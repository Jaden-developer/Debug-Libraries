/*
  Created by Jaden Wester
  Copyright © Jaden Wester inc 2019. 
  All rights reserved!
*/

static int currentpc (lua_State *L, CallInfo *ci) {
  if (!isLua(ci)) return -1;  /* function is not a Lua function? */
  if (ci == L->ci)
    ci->savedpc = L->savedpc;
  return pcRel(ci->savedpc, ci_func(ci)->l.p);
}


static int currentline (lua_State *L, CallInfo *ci) {
  int pc = currentpc(L, ci);
  if (pc < 0)
    return -1;  /* only active lua functions have current-line information */
  else
    return getline(ci_func(ci)->l.p, pc);
}


/*
** this function can be called asynchronous (e.g. during a signal)
*/
int lua_sethook (lua_State *L, lua_Hook func, int mask, int count) {
  if (func == NULL || mask == 0) {  /* turn off hooks? */
    mask = 0;
    func = NULL;
  }
  L->hook = func;
  L->basehookcount = count;
  resethookcount(L);
  L->hookmask = cast_byte(mask);
  return 1;
}


lua_Hook lua_gethook (lua_State *L) {
  return L->hook;
}


int lua_gethookmask (lua_State *L) {
  return L->hookmask;
}


int lua_gethookcount (lua_State *L) {
  return L->basehookcount;
}


int lua_getstack (lua_State *L, int level, lua_Debug *ar) {
  int status;
  CallInfo *ci;
  lua_lock(L);
  for (ci = L->ci; level > 0 && ci > L->base_ci; ci--) {
    level--;
    if (f_isLua(ci))  /* Lua function? */
      level -= ci->tailcalls;  /* skip lost tail calls */
  }
  if (level == 0 && ci > L->base_ci) {  /* level found? */
    status = 1;
    ar->i_ci = cast_int(ci - L->base_ci);
  }
  else if (level < 0) {  /* level is of a lost tail call? */
    status = 1;
    ar->i_ci = 0;
  }
  else status = 0;  /* no such level */
  lua_unlock(L);
  return status;
}


static Proto *getluaproto (CallInfo *ci) {
  return (isLua(ci) ? ci_func(ci)->l.p : NULL);
}


static const char *findlocal (lua_State *L, CallInfo *ci, int n) {
  const char *name;
  Proto *fp = getluaproto(ci);
  if (fp && (name = luaF_getlocalname(fp, n, currentpc(L, ci))) != NULL)
    return name;  /* is a local variable in a Lua function */
  else {
    StkId limit = (ci == L->ci) ? L->top : (ci+1)->func;
    if (limit - ci->base >= n && n > 0)  /* is 'n' inside 'ci' stack? */
      return "(*temporary)";
    else
      return NULL;
  }
}


const char *lua_getlocal (lua_State *L, const lua_Debug *ar, int n) {
  CallInfo *ci = L->base_ci + ar->i_ci;
  const char *name = findlocal(L, ci, n);
  lua_lock(L);
  if (name)
      luaA_pushobject(L, ci->base + (n - 1));
  lua_unlock(L);
  return name;
}


const char *lua_setlocal (lua_State *L, const lua_Debug *ar, int n) {
  CallInfo *ci = L->base_ci + ar->i_ci;
  const char *name = findlocal(L, ci, n);
  lua_lock(L);
  if (name)
      setobjs2s(L, ci->base + (n - 1), L->top - 1);
  L->top--;  /* pop value */
  lua_unlock(L);
  return name;
}


static void funcinfo (lua_Debug *ar, Closure *cl) {
  if (cl->c.isC) {
    ar->source = "=[C]";
    ar->linedefined = -1;
    ar->lastlinedefined = -1;
    ar->what = "C";
  }
  else {
    ar->source = getstr(cl->l.p->source);
    ar->linedefined = cl->l.p->linedefined;
    ar->lastlinedefined = cl->l.p->lastlinedefined;
    ar->what = (ar->linedefined == 0) ? "main" : "Lua";
  }
  luaO_chunkid(ar->short_src, ar->source, LUA_IDSIZE);
}


static void info_tailcall (lua_Debug *ar) {
  ar->name = ar->namewhat = "";
  ar->what = "tail";
  ar->lastlinedefined = ar->linedefined = ar->currentline = -1;
  ar->source = "=(tail call)";
  luaO_chunkid(ar->short_src, ar->source, LUA_IDSIZE);
  ar->nups = 0;
}


static void collectvalidlines (lua_State *L, Closure *f) {
  if (f == NULL || f->c.isC) {
    setnilvalue(L->top);
  }
  else {
    Table *t = luaH_new(L, 0, 0);
    int *lineinfo = f->l.p->lineinfo;
    int i;
    for (i=0; i<f->l.p->sizelineinfo; i++)
      setbvalue(luaH_setnum(L, t, lineinfo[i]), 1);
    sethvalue(L, L->top, t); 
  }
  incr_top(L);
}


static int auxgetinfo (lua_State *L, const char *what, lua_Debug *ar,
                    Closure *f, CallInfo *ci) {
  int status = 1;
  if (f == NULL) {
    info_tailcall(ar);
    return status;
  }
  for (; *what; what++) {
    switch (*what) {
      case 'S': {
        funcinfo(ar, f);
        break;
      }
      case 'l': {
        ar->currentline = (ci) ? currentline(L, ci) : -1;
        break;
      }
      case 'u': {
        ar->nups = f->c.nupvalues;
        break;
      }
      case 'n': {
        ar->namewhat = (ci) ? getfuncname(L, ci, &ar->name) : NULL;
        if (ar->namewhat == NULL) {
          ar->namewhat = "";  /* not found */
          ar->name = NULL;
        }
        break;
      }
      case 'L':
      case 'f':  /* handled by lua_getinfo */
        break;
      default: status = 0;  /* invalid option */
    }
  }
  return status;
}


int lua_getinfo (lua_State *L, const char *what, lua_Debug *ar) {
  int status;
  Closure *f = NULL;
  CallInfo *ci = NULL;
  lua_lock(L);
  if (*what == '>') {
    StkId func = L->top - 1;
    luai_apicheck(L, ttisfunction(func));
    what++;  /* skip the '>' */
    f = clvalue(func);
    L->top--;  /* pop function */
  }
  else if (ar->i_ci != 0) {  /* no tail call? */
    ci = L->base_ci + ar->i_ci;
    lua_assert(ttisfunction(ci->func));
    f = clvalue(ci->func);
  }
  status = auxgetinfo(L, what, ar, f, ci);
  if (strchr(what, 'f')) {
    if (f == NULL) setnilvalue(L->top);
    else setclvalue(L, L->top, f);
    incr_top(L);
  }
  if (strchr(what, 'L'))
    collectvalidlines(L, f);
  lua_unlock(L);
  return status;
}
