#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <ctype.h>
#include <cstdlib>
#include <string.h>

#include <signal.h>

#include <iocsh.h>
#include <shareLib.h>
#include <epicsExport.h>
#include <epicsReadline.h>
#include <epicsThread.h>

#include "luaEpics.h"
#include "luaShell.h"

/* mark in error messages for incomplete statements */
#define EOFMARK		"<eof>"
#define marklen		(sizeof(EOFMARK)/sizeof(char) - 1)

#if !defined(LUA_PROGNAME)
#define LUA_PROGNAME		"lua"
#endif


#if defined(__vxworks) || defined(vxWorks)
	#include <symLib.h>
	#include <sysSymTbl.h>
#endif

static lua_State *globalL = NULL;
static const char *progname = LUA_PROGNAME;

static lua_State* shell_state = NULL;

static void luashBody(lua_State* state, const char* pathname);


/*
** Hook set by signal function to stop the interpreter.
*/
static void lstop (lua_State *L, lua_Debug *ar)
{
	(void)ar;  /* unused arg. */
	lua_sethook(L, NULL, 0, 0);  /* reset hook */
	luaL_error(L, "interrupted!");
}

/*
** Function to be called at a C signal. Because a C signal cannot
** just change a Lua state (as there is no proper synchronization),
** this function only sets a hook that, when called, will stop the
** interpreter.
*/
static void laction (int i)
{
	signal(i, SIG_DFL); /* if another SIGINT happens, terminate process */
	lua_sethook(globalL, lstop, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


/*
** Prints an error message, adding the program name in front of it
** (if present)
*/
static void l_message (const char *pname, const char *msg)
{
	if (pname)    { lua_writestringerror("%s: ", pname); }

	lua_writestringerror("%s\n", msg);
}


/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack. It assumes that the error object
** is a string, as it was either generated by Lua or by 'msghandler'.
*/
static int report (lua_State *L, int status)
{
	if (status != LUA_OK)
	{
		const char *msg = lua_tostring(L, -1);
		l_message(progname, msg);
		lua_pop(L, 1);  /* remove message */
	}

	return status;
}

/*
** Message handler used to run all chunks
*/
static int msghandler (lua_State *L)
{
	const char *msg = lua_tostring(L, 1);
	if (msg == NULL)  /* is error object not a string? */
	{
		if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
		    lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
		{
			return 1;  /* that is the message */
		}
		else
		{
			msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
		}
	}

	luaL_traceback(L, L, msg, 1);  /* append a standard traceback */

	return 1;  /* return the traceback */
}


/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void l_print (lua_State *L)
{
	int n = lua_gettop(L);

	if (n > 0)  /* any result to be printed? */
	{
		luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
		lua_getglobal(L, "print");
		lua_insert(L, 1);

		if (lua_pcall(L, n, 0, 0) != LUA_OK)
		{
			l_message(progname, lua_pushfstring(L, "error calling 'print' (%s)", lua_tostring(L, -1)));
		}
	}
}


/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete (lua_State *L, int status)
{
	if (status == LUA_ERRSYNTAX)
	{
		size_t lmsg;
		const char *msg = lua_tolstring(L, -1, &lmsg);

		if (lmsg >= marklen && strcmp(msg + lmsg - marklen, EOFMARK) == 0)
		{
			lua_pop(L, 1);
			return 1;
		}
	}

	return 0;  /* else... */
}

/*
** Try to compile line on the stack as 'return <line>'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int addreturn (lua_State *L)
{
	int status;
	size_t len; const char *line;
	lua_pushliteral(L, "return ");
	lua_pushvalue(L, -2);  /* duplicate line */
	lua_concat(L, 2);  /* new line is "return ..." */
	line = lua_tolstring(L, -1, &len);

	if ((status = luaL_loadbuffer(L, line, len, "=stdin")) == LUA_OK)
	{
		lua_remove(L, -3);  /* remove original line */
	}
	else
	{
		lua_pop(L, 2);  /* remove result from 'luaL_loadbuffer' and new line */
	}

	return status;
}

/*
** Read multiple lines until a complete Lua statement
*/
static int multiline (lua_State *L, const char* prompt, void* readlineContext)
{
	const char* subprompt = prompt ? "> " : "";

	for (;;)  /* repeat until gets a complete statement */
	{
		size_t len;
		const char *buffer = lua_tolstring(L, 1, &len);  /* get what it has */
		int status = luaL_loadbuffer(L, buffer, len, "=stdin");  /* try it */

		/* cannot or should not try to add continuation line */
		if (!incomplete(L, status))    { return status; }

		const char* raw = epicsReadline(subprompt, readlineContext);

		if (!raw)       { return status; }
		
		std::string line(raw);
		while (line.length() > 0 && isspace(line[0]))    { line.erase(0,1); }
		
		if (!prompt)    { printf("%s\n", raw); }

		lua_pushstring(L, line.c_str());
		lua_pushliteral(L, "\n");  /* add newline... */
		lua_insert(L, -2);  /* ...between the two lines */
		lua_concat(L, 3);  /* join them */
	}
}


/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/
static int docall (lua_State *L, int narg, int nres)
{
	int status;
	int base = lua_gettop(L) - narg;   /* function index */
	lua_pushcfunction(L, msghandler);  /* push message handler */
	lua_insert(L, base);               /* put it under function and args */
	globalL = L;                       /* to be available to 'laction' */
	signal(SIGINT, laction);           /* set C-signal handler */
	status = lua_pcall(L, narg, nres, base);
	signal(SIGINT, SIG_DFL);          /* reset C-signal handler */
	lua_remove(L, base);              /* remove message handler from the stack */

	return status;
}

static void repl(lua_State* state, void* readlineContext, const char* prompt)
{
	int status;
	
	while (true)
	{
		/*
		** Read a line and try to load (compile) it first as an expression (by
		** adding "return " in front of it) and second as a statement. Return
		** the final status of load/call with the resulting function (if any)
		** in the top of the stack.
		*/
		lua_settop(state, 0);

		const char* raw = epicsReadline(prompt, readlineContext);
		
		if (raw == NULL)                 { return; }
		
		std::string line(raw);
		
		// Eliminate leading white space
		while (line.length() > 0 && isspace(line[0]))    { line.erase(0,1); }
		
		if (line == "exit")    { return; }

		if (line.length() > 0 && line[0] == '<')
		{
			// Get rid of < character
			line.erase(0,1);

			// Get rid of whitespace
			while (line.length() > 0 && isspace(line[0]))    { line.erase(0,1); }

			luashBody(state, line.c_str());
			continue;
		}
		
		if (line.length() > 0 && line[0] == '#')
		{
			lua_getglobal(state, "enableHashComments");
			const char* yn = lua_tostring(state, -1);
			lua_pop(state, 1);
			if (yn && std::string(yn) == "YES")
			{
				if (prompt == NULL)    { printf("%s\n", raw); }
				continue;
			}
		}

		lua_pushstring(state, line.c_str());

		if (prompt == NULL)    { printf("%s\n", raw); }

		/* try as command, maybe with continuation lines */
		if ((status = addreturn(state)) != LUA_OK)    { status = multiline(state, prompt, readlineContext); }

		lua_remove(state, 1);  /* remove line from the stack */
		lua_assert(lua_gettop(state) == 1);

		if (status == LUA_OK)     { status = docall(state, 0, LUA_MULTRET); }

		if (status == LUA_OK)     { l_print(state); }
		else                      { report(state, status); }
	}
}



static void luashBody(lua_State* state, const char* pathname)
{
	int wasOkToBlock;

	const char* prompt = NULL;
	void* readlineContext = NULL;

	FILE *fp = NULL;

	if (pathname)
	{
		std::string filename(pathname);
		std::string path = luaLocateFile(filename);

		if (path.empty())    { printf("File %s not found\n", pathname); return; }

		fp = fopen(path.c_str(), "r");

		readlineContext = epicsReadlineBegin(fp);
	}
	else
	{
		readlineContext = epicsReadlineBegin(NULL);

		prompt = std::getenv("LUASH_PS1");

		#if defined(__vxworks) || defined(vxWorks)
		/* For compatibility reasons look for global symbols */
		if (prompt == NULL)
		{
			char* symbol;
			SYM_TYPE type;

			if (symFindByName(sysSymTbl, "LUASH_PS1", &symbol, &type) == OK)
			{
				prompt = *(char**) symbol;
			}
		}
		#endif

		if (prompt == NULL)    { prompt = "luash> "; }
	}


	wasOkToBlock = epicsThreadIsOkToBlock();
    epicsThreadSetOkToBlock(1);

	if (readlineContext)    { repl(state, readlineContext, prompt);	}
	else                    { printf("Couldn't allocate command-line object.\n"); }

	if (fp)    { fclose(fp); }

	epicsThreadSetOkToBlock( wasOkToBlock);
	epicsReadlineEnd(readlineContext);
	lua_settop(state, 0);  /* clear stack */
}

void spawn_thread_callback(void* arg)
{
	lua_State* state = (lua_State*) arg;
	
	int status = docall(state, 0, 0);
	
	if (status != LUA_OK)    { report(state, status); }
	
	lua_close(state);
}

static LUA_LIBRARY_LOAD_HOOK_ROUTINE previousLibraryHook=NULL;
static LUA_FUNCTION_LOAD_HOOK_ROUTINE previousFunctionHook=NULL;

static void newLibraryLoadedHook(const char* library_name, lua_CFunction library_func)
{
	if (previousLibraryHook)    { previousLibraryHook(library_name, library_func); }
	
	luaL_requiref(shell_state, library_name, library_func, 1);
}

static void newFunctionLoadedHook(const char* function_name, lua_CFunction function)
{
	if (previousFunctionHook)    { previousFunctionHook(function_name, function); }
	
	lua_register(shell_state, function_name, function);
}


static const iocshArg luashCmdArg0 = { "lua shell script", iocshArgString};
static const iocshArg luashCmdArg1 = { "macros", iocshArgString};
static const iocshArg *luashCmdArgs[2] = {&luashCmdArg0, &luashCmdArg1};
static const iocshFuncDef luashFuncDef = {"luash", 2, luashCmdArgs};

static void luashCallFunc(const iocshArgBuf* args)
{
	luashBegin(args[0].sval, args[1].sval);
}

static const iocshArg luaCmdCmdArg0 = { "lua script", iocshArgString};
static const iocshArg luaCmdCmdArg1 = { "macros", iocshArgString};
static const iocshArg *luaCmdCmdArgs[2] = {&luaCmdCmdArg0, &luaCmdCmdArg1};
static const iocshFuncDef luaCmdFuncDef = {"luaCmd", 2, luaCmdCmdArgs};


static void luaCmdCallFunc(const iocshArgBuf* args)
{
	luaCmd(args[0].sval, args[1].sval);
}

static const iocshArg spawnCmdArg0 = { "lua script", iocshArgString};
static const iocshArg spawnCmdArg1 = { "macros", iocshArgString};
static const iocshArg *spawnCmdArgs[2] = {&spawnCmdArg0, &spawnCmdArg1};
static const iocshFuncDef spawnFuncDef = {"luaSpawn", 2, spawnCmdArgs};


static void spawnCallFunc(const iocshArgBuf* args)
{
	luaSpawn(args[0].sval, args[1].sval);
}

extern "C"
{

epicsShareFunc int epicsShareAPI luashBegin(const char* pathname, const char* macros)
{
	if (shell_state != NULL)
	{
		luashBody(shell_state, pathname);
		return 0;
	}
	
	shell_state = luaCreateState();
	
	lua_getglobal(shell_state, "_G");
	luaL_setmetatable(shell_state, "iocsh_meta");
	lua_pop(shell_state, 1);

	lua_pushlightuserdata(shell_state, *iocshPpdbbase);
	lua_setglobal(shell_state, "pdbbase");

	if (macros)    { luaLoadMacros(shell_state, macros); }

	previousLibraryHook = luaLoadLibraryHook;
	previousFunctionHook = luaLoadFunctionHook;
	
	luaLoadLibraryHook = newLibraryLoadedHook;
	luaLoadFunctionHook = newFunctionLoadedHook;
	
	luashBody(shell_state, pathname);
	
	lua_close(shell_state);
	
	shell_state = NULL;

	return 0;
}

epicsShareFunc int epicsShareAPI luash(const char* pathname)
{
	return luashBegin(pathname, NULL);
}

epicsShareFunc int epicsShareAPI luaCmd(const char* command, const char* macros)
{
	if (! command)    { return -1; }
	
	lua_State* state = luaCreateState();
	
	lua_getglobal(state, "_G");
	luaL_setmetatable(state, "iocsh_meta");
	lua_pop(state, 1);
	
	if (macros)
	{
		luaLoadMacros(state, macros);
	}
	
	int status = luaL_loadbuffer(state, command, strlen(command), "=stdin");
	
	if (status == LUA_OK)     { status = docall(state, 0, LUA_MULTRET); }

	if (status == LUA_OK)     { l_print(state); }
	else                      { report(state, status); }
	
	lua_close(state);
	
	return 0;
}

epicsShareFunc int epicsShareAPI luaSpawn(const char* filename, const char* macros)
{
	lua_State* state = luaCreateState();
	
	if (macros)
	{
		luaLoadMacros(state, macros);
	}
	
	std::string temp(filename);
	std::string found = luaLocateFile(temp);
	
	if (found.empty())    { return -1; }
	
	int status = luaL_loadfile(state, found.c_str());
	
	if (status)
	{
		std::string err(lua_tostring(state, -1));
		lua_pop(state, 1);
		
		printf("%s\n", err.c_str());
		return status;
	}
	
	std::stringstream temp_stream;
	std::string threadname;
	
	temp_stream << "Lua Shell Thread: " << filename << "(" << macros << ")";
	temp_stream >> threadname;
	
	epicsThreadCreate(threadname.c_str(), 
	                  epicsThreadPriorityLow, 
	                  epicsThreadGetStackSize(epicsThreadStackMedium), 
	                  (EPICSTHREADFUNC)::spawn_thread_callback, state);
					
	return 0;
}


static void luashRegister(void)
{
	iocshRegister(&luashFuncDef, luashCallFunc);
	iocshRegister(&spawnFuncDef, spawnCallFunc);
	iocshRegister(&luaCmdFuncDef, luaCmdCallFunc);
}

epicsExportRegistrar(luashRegister);
}

