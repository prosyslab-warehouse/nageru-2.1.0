#ifndef _LUA_UTILS_H
#define _LUA_UTILS_H 1

#include <lauxlib.h>
#include <lua.hpp>

#include <mutex>
#include <utility>

class LuaRefWithDeleter {
public:
	LuaRefWithDeleter(std::mutex *m, lua_State *L, int ref) : m(m), L(L), ref(ref) {}
	~LuaRefWithDeleter() {
		std::lock_guard<std::mutex> lock(*m);
		luaL_unref(L, LUA_REGISTRYINDEX, ref);
	}
	int get() const { return ref; }

private:
	LuaRefWithDeleter(const LuaRefWithDeleter &) = delete;

	std::mutex *m;
	lua_State *L;
	int ref;
};

template<class T, class... Args>
int wrap_lua_object(lua_State* L, const char *class_name, Args&&... args)
{
	// Construct the C++ object and put it on the stack.
	void *mem = lua_newuserdata(L, sizeof(T));
	new(mem) T(std::forward<Args>(args)...);

	// Look up the metatable named <class_name>, and set it on the new object.
	luaL_getmetatable(L, class_name);
	lua_setmetatable(L, -2);

	return 1;
}

// Like wrap_lua_object, but the object is not owned by Lua; ie. it's not freed
// by Lua GC. This is typically the case for Effects, which are owned by EffectChain
// and expected to be destructed by it. The object will be of type T** instead of T*
// when exposed to Lua.
//
// Note that we currently leak if you allocate an Effect in this way and never call
// add_effect. We should see if there's a way to e.g. set __gc on it at construction time
// and then release that once add_effect() takes ownership.
template<class T, class... Args>
int wrap_lua_object_nonowned(lua_State* L, const char *class_name, Args&&... args)
{
	// Construct the pointer ot the C++ object and put it on the stack.
	T **obj = (T **)lua_newuserdata(L, sizeof(T *));
	*obj = new T(std::forward<Args>(args)...);

	// Look up the metatable named <class_name>, and set it on the new object.
	luaL_getmetatable(L, class_name);
	lua_setmetatable(L, -2);

	return 1;
}

template<class T>
int wrap_lua_existing_object_nonowned(lua_State* L, const char *class_name, T *ptr)
{
	// Construct the pointer ot the C++ object and put it on the stack.
	T **obj = (T **)lua_newuserdata(L, sizeof(T *));
	*obj = ptr;

	// Look up the metatable named <class_name>, and set it on the new object.
	luaL_getmetatable(L, class_name);
	lua_setmetatable(L, -2);

	return 1;
}

#endif  // !defined(_LUA_UTILS_H)
