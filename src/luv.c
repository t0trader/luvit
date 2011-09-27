#include "luv.h"
#include "uv.h"
#include <stdlib.h>
#include <assert.h>

////////////////////////////////////////////////////////////////////////////////
//                              ref structs                                   //
////////////////////////////////////////////////////////////////////////////////

typedef struct {
  lua_State* L;
  int r;
} luv_ref_t;

typedef struct {
  lua_State* L;
  int r;
  uv_write_t write_req;
  uv_buf_t refbuf;
} luv_write_ref_t;

////////////////////////////////////////////////////////////////////////////////
//                             utility functions                              //
////////////////////////////////////////////////////////////////////////////////

// Registers a callback, callback_index can't be negative
static void luv_register_event(lua_State* L, int userdata_index, const char* name, int callback_index) {
  int before = lua_gettop(L);
  lua_getfenv(L, userdata_index);
  lua_pushvalue(L, callback_index);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  assert(lua_gettop(L) == before);
}

// Emit an event of the current userdata consuming nargs
// Assumes userdata is right below args
static void luv_emit_event(lua_State* L, const char* name, int nargs) {
  int before = lua_gettop(L);
  // Load the connection callback
  lua_getfenv(L, -nargs - 1);
  lua_getfield(L, -1, name);
  lua_remove(L, -2);
  if (lua_isfunction (L, -1) == 0) {
    //printf("missing event: on_%s\n", name);
    lua_pop(L, 1 + nargs);
    assert(lua_gettop(L) == before - nargs);
    return;
  }

  // move the function below the args
  lua_insert(L, -nargs - 1);
  if (lua_pcall(L, nargs, 0, 0) != 0) {
    error(L, "error running function 'on_%s': %s", name, lua_tostring(L, -1));
  }
  assert(lua_gettop(L) == before - nargs);
}

////////////////////////////////////////////////////////////////////////////////
//                        event callback dispatchers                          //
////////////////////////////////////////////////////////////////////////////////

void luv_on_connection(uv_stream_t* handle, int status) {
  // load the lua state and the userdata
  luv_ref_t* ref = handle->data;
  lua_State *L = ref->L;
  int before = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref->r);

  lua_pushinteger(L, status);
  luv_emit_event(L, "connection", 1);
  lua_pop(L, 1); // remove the userdata
  assert(lua_gettop(L) == before);
}

uv_buf_t luv_on_alloc(uv_handle_t* handle, size_t suggested_size) {
  uv_buf_t buf;
  buf.base = malloc(suggested_size);
  buf.len = suggested_size;
  return buf;
}

void luv_on_close(uv_handle_t* handle) {

  // load the lua state and the userdata
  luv_ref_t* ref = handle->data;
  lua_State *L = ref->L;
  int before = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref->r);

  luv_emit_event(L, "closed", 0);
  lua_pop(L, 1); // remove userdata

  // This handle is no longer valid, clean up memory
  luaL_unref(L, LUA_REGISTRYINDEX, ref->r);
  free(ref);

  assert(lua_gettop(L) == before);
}

void luv_on_read(uv_stream_t* handle, ssize_t nread, uv_buf_t buf) {

  // load the lua state and the userdata
  luv_ref_t* ref = handle->data;
  lua_State *L = ref->L;
  int before = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref->r);

  if (nread >= 0) {

    lua_pushlstring (L, buf.base, nread);
    lua_pushinteger (L, nread);
    luv_emit_event(L, "read", 2);

  } else {
    uv_err_t err = uv_last_error(uv_default_loop());
    if (err.code == UV_EOF) {
      luv_emit_event(L, "end", 0);
    } else {
      error(L, "read: %s", uv_strerror(err));
    }
  }
  lua_pop(L, 1); // Remove the userdata

  free(buf.base);
  assert(lua_gettop(L) == before);
}

void luv_after_write(uv_write_t* req, int status) {

  // load the lua state and the callback
  luv_ref_t* ref = req->data;
  lua_State *L = ref->L;
  int before = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref->r);
  luaL_unref(L, LUA_REGISTRYINDEX, ref->r);
  if (lua_pcall(L, 0, 0, 0) != 0) {
    error(L, "error running function 'on_write': %s", lua_tostring(L, -1));
  }
  free(ref);// We're done with the ref object, free it
  assert(lua_gettop(L) == before);

}

////////////////////////////////////////////////////////////////////////////////
//                           wrapped uv functions                             //
////////////////////////////////////////////////////////////////////////////////

static int luv_run (lua_State* L) {
  uv_run(uv_default_loop());
  return 0;
}

static int luv_tcp_init (lua_State* L) {
  int before = lua_gettop(L);
  uv_tcp_t* handle = (uv_tcp_t*)luaL_checkudata(L, 1, "luv_tcp");
  uv_tcp_init(uv_default_loop(), handle);
  assert(lua_gettop(L) == before);
  return 0;
}

static int luv_tcp_bind (lua_State* L) {
  int before = lua_gettop(L);
  uv_tcp_t* handle = (uv_tcp_t*)luaL_checkudata(L, 1, "luv_tcp");
  const char* host = luaL_checkstring(L, 2);
  int port = luaL_checkint(L, 3);

  struct sockaddr_in address = uv_ip4_addr(host, port);

  if (uv_tcp_bind(handle, address)) {
    uv_err_t err = uv_last_error(uv_default_loop());
    error(L, "tcp_bind: %s", uv_strerror(err));
  }

  assert(lua_gettop(L) == before);
  return 0;
}

static int luv_listen (lua_State* L) {
  int before = lua_gettop(L);
  uv_stream_t* handle = (uv_stream_t*)luaL_checkudata(L, 1, "luv_tcp");
  luaL_checktype(L, 2, LUA_TFUNCTION);

  luv_register_event(L, 1, "connection", 2);

  if (uv_listen(handle, 128, luv_on_connection)) {
    uv_err_t err = uv_last_error(uv_default_loop());
    error(L, "listen: %s", uv_strerror(err));
  }

  assert(lua_gettop(L) == before);
  return 0;
}

static int luv_accept (lua_State* L) {
  int before = lua_gettop(L);
  uv_stream_t* server = (uv_stream_t*)luaL_checkudata(L, 1, "luv_tcp");
  uv_stream_t* client = (uv_stream_t*)luaL_checkudata(L, 2, "luv_tcp");
  if (uv_accept(server, client)) {
    uv_err_t err = uv_last_error(uv_default_loop());
    error(L, "accept: %s", uv_strerror(err));
  }

  assert(lua_gettop(L) == before);
  return 0;
}

static int luv_close (lua_State* L) {
  int before = lua_gettop(L);
  uv_tcp_t* handle = (uv_tcp_t*)luaL_checkudata(L, 1, "luv_tcp");
  uv_close((uv_handle_t*)handle, luv_on_close);
  assert(lua_gettop(L) == before);
  return 0;
}

static int luv_read_start (lua_State* L) {
  int before = lua_gettop(L);
  uv_stream_t* handle = (uv_stream_t*)luaL_checkudata(L, 1, "luv_tcp");
  uv_read_start(handle, luv_on_alloc, luv_on_read);
  assert(lua_gettop(L) == before);
  return 0;
}

static int luv_write (lua_State* L) {
  int before = lua_gettop(L);
  uv_stream_t* handle = (uv_stream_t*)luaL_checkudata(L, 1, "luv_tcp");
  size_t len;
  const char* chunk = luaL_checklstring(L, 2, &len);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  luv_write_ref_t* ref = (luv_write_ref_t*)malloc(sizeof(luv_write_ref_t));

  // Store a reference to the callback
  ref->L = L;
  lua_pushvalue(L, 3);
  ref->r = luaL_ref(L, LUA_REGISTRYINDEX);

  // Give the write_req access to this
  ref->write_req.data = ref;

  // Store the chunk
  // TODO: this is probably unsafe, should investigate
  ref->refbuf.base = (char*)chunk;
  ref->refbuf.len = len;

  uv_write(&ref->write_req, handle, &ref->refbuf, 1, luv_after_write);
  assert(lua_gettop(L) == before);
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
//                             uv constructors                                //
////////////////////////////////////////////////////////////////////////////////

static int luv_new_tcp (lua_State* L) {
  int before = lua_gettop(L);

  uv_tcp_t* handle = (uv_tcp_t*)lua_newuserdata(L, sizeof(uv_tcp_t));

  // Store a reference to the userdata in the handle
  luv_ref_t* ref = (luv_ref_t*)malloc(sizeof(luv_ref_t));
  ref->L = L;
  lua_pushvalue(L, -1); // duplicate so we can _ref it
  ref->r = luaL_ref(L, LUA_REGISTRYINDEX);
  handle->data = ref;

  uv_tcp_init(uv_default_loop(), handle);

  // Set metatable for type
  luaL_getmetatable(L, "luv_tcp");
  lua_setmetatable(L, -2);

  // Create a local environment for storing stuff
  lua_newtable(L);
  lua_setfenv (L, -2);

  assert(lua_gettop(L) == before + 1);
  // return the userdata
  return 1;
}


////////////////////////////////////////////////////////////////////////////////
//                              custom APIs                                   //
////////////////////////////////////////////////////////////////////////////////

static int luv_set_handler(lua_State* L) {
  int before = lua_gettop(L);
  luaL_checkudata(L, 1, "luv_tcp");
  const char* name = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TFUNCTION);

  luv_register_event(L, 1, name, 3);

  assert(lua_gettop(L) == before);
  return 0;
}

////////////////////////////////////////////////////////////////////////////////

static const luaL_reg luv_f[] = {
  {"new_tcp", luv_new_tcp},
  {"tcp_init", luv_tcp_init},
  {"tcp_bind", luv_tcp_bind},
  {"listen", luv_listen},
  {"accept", luv_accept},
  {"write", luv_write},
  {"close", luv_close},
  {"read_start", luv_read_start},
  {"run", luv_run},
  {"set_handler", luv_set_handler},
  {NULL, NULL}
};


LUALIB_API int luaopen_uv (lua_State* L) {
  int before = lua_gettop(L);

  // Create the luv_tcp userdata type
  luaL_newmetatable(L, "luv_tcp");
  lua_pop(L, 1);

  // Create a new exports table with functions and constants
  lua_newtable (L);
  luaL_register(L, NULL, luv_f);
  lua_pushnumber(L, UV_VERSION_MAJOR);
  lua_setfield(L, -2, "VERSION_MAJOR");
  lua_pushnumber(L, UV_VERSION_MINOR);
  lua_setfield(L, -2, "VERSION_MINOR");

  assert(lua_gettop(L) == before + 1);
  return 1;
}

