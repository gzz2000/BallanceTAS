#include "LuaApi.h"

#include "../LuaRuntime/LuaStackGuard.h"
#include "../LuaRuntime/LuaUserdata.h"

#include <VxMath.h>
#include <cstdio>

constexpr const char *kVxColorMt = "BallanceTAS.VxColor";

static VxColor *CheckVxColor(lua_State *L, int index) {
    return tas::lua::CheckUserdata<VxColor>(L, index, kVxColorMt);
}

static VxColor *TestVxColor(lua_State *L, int index) {
    auto *box = static_cast<tas::lua::UserdataBox<VxColor> *>(luaL_testudata(L, index, kVxColorMt));
    return box ? box->ptr : nullptr;
}

static void PushVxColor(lua_State *L, const VxColor &value) {
    tas::lua::PushOwnedUserdata<VxColor>(L, kVxColorMt, value);
}

static void PushVxColor(lua_State *L, float r, float g, float b, float a) {
    PushVxColor(L, VxColor(r, g, b, a));
}

static int VxColorNew(lua_State *L) {
    const int argc = lua_gettop(L);
    if (argc == 0) {
        PushVxColor(L, VxColor());
        return 1;
    }
    if (argc == 1) {
        PushVxColor(L, VxColor(static_cast<float>(luaL_checknumber(L, 1))));
        return 1;
    }
    if (argc == 3 || argc == 4) {
        const float r = static_cast<float>(luaL_checknumber(L, 1));
        const float g = static_cast<float>(luaL_checknumber(L, 2));
        const float b = static_cast<float>(luaL_checknumber(L, 3));
        const float a = argc == 4 ? static_cast<float>(luaL_checknumber(L, 4)) : 1.0f;
        PushVxColor(L, VxColor(r, g, b, a));
        return 1;
    }
    return luaL_error(L, "VxColor.new: expected 0, 1, 3, or 4 numeric arguments");
}

static int VxColorCall(lua_State *L) {
    lua_remove(L, 1);
    return VxColorNew(L);
}

static int VxColorRGBA(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(color->GetRGBA()));
    return 1;
}

static int VxColorSetRGBA(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    color->Set(static_cast<XDWORD>(luaL_checkinteger(L, 2)));
    return 0;
}

static int VxColorRGB(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(color->GetRGB()));
    return 1;
}

static int VxColorRed(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(ColorGetRed(color->GetRGBA())));
    return 1;
}

static int VxColorSetRed(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    color->Set(static_cast<XDWORD>(ColorSetRed(color->GetRGBA(), static_cast<XDWORD>(luaL_checkinteger(L, 2)))));
    return 0;
}

static int VxColorGreen(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(ColorGetGreen(color->GetRGBA())));
    return 1;
}

static int VxColorSetGreen(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    color->Set(static_cast<XDWORD>(ColorSetGreen(color->GetRGBA(), static_cast<XDWORD>(luaL_checkinteger(L, 2)))));
    return 0;
}

static int VxColorBlue(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(ColorGetBlue(color->GetRGBA())));
    return 1;
}

static int VxColorSetBlue(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    color->Set(static_cast<XDWORD>(ColorSetBlue(color->GetRGBA(), static_cast<XDWORD>(luaL_checkinteger(L, 2)))));
    return 0;
}

static int VxColorAlpha(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    lua_pushinteger(L, static_cast<lua_Integer>(ColorGetAlpha(color->GetRGBA())));
    return 1;
}

static int VxColorSetAlpha(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    color->Set(static_cast<XDWORD>(ColorSetAlpha(color->GetRGBA(), static_cast<XDWORD>(luaL_checkinteger(L, 2)))));
    return 0;
}

static int VxColorClear(lua_State *L) {
    CheckVxColor(L, 1)->Clear();
    lua_settop(L, 1);
    return 1;
}

static int VxColorCheck(lua_State *L) {
    CheckVxColor(L, 1)->Check();
    lua_settop(L, 1);
    return 1;
}

static int VxColorSet(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    const int argc = lua_gettop(L) - 1;
    if (argc == 1) {
        color->Set(static_cast<float>(luaL_checknumber(L, 2)));
        lua_settop(L, 1);
        return 1;
    }
    if (argc == 3 || argc == 4) {
        const float r = static_cast<float>(luaL_checknumber(L, 2));
        const float g = static_cast<float>(luaL_checknumber(L, 3));
        const float b = static_cast<float>(luaL_checknumber(L, 4));
        if (argc == 4) {
            color->Set(r, g, b, static_cast<float>(luaL_checknumber(L, 5)));
        } else {
            color->Set(r, g, b);
        }
        // Return self, like VxVector:set.
        lua_settop(L, 1);
        return 1;
    }
    return luaL_error(L, "VxColor:set: expected 1, 3, or 4 numeric arguments");
}

static int VxColorSquareDistance(lua_State *L) {
    auto *a = CheckVxColor(L, 1);
    auto *b = CheckVxColor(L, 2);
    lua_pushnumber(L, a->GetSquareDistance(*b));
    return 1;
}

static int VxColorConvert(lua_State *L) {
    const int argc = lua_gettop(L);
    if (argc == 3 || argc == 4) {
        const float r = static_cast<float>(luaL_checknumber(L, 1));
        const float g = static_cast<float>(luaL_checknumber(L, 2));
        const float b = static_cast<float>(luaL_checknumber(L, 3));
        const unsigned long value = argc == 4
                                        ? VxColor::Convert(r, g, b, static_cast<float>(luaL_checknumber(L, 4)))
                                        : VxColor::Convert(r, g, b);
        lua_pushinteger(L, static_cast<lua_Integer>(value));
        return 1;
    }
    return luaL_error(L, "VxColor.convert: expected 3 or 4 numeric arguments");
}

static int VxColorAdd(lua_State *L) {
    auto *a = CheckVxColor(L, 1);
    auto *b = CheckVxColor(L, 2);
    PushVxColor(L, *a + *b);
    return 1;
}

static int VxColorSub(lua_State *L) {
    auto *a = CheckVxColor(L, 1);
    auto *b = CheckVxColor(L, 2);
    PushVxColor(L, *a - *b);
    return 1;
}

static int VxColorMul(lua_State *L) {
    auto *a = TestVxColor(L, 1);
    auto *b = TestVxColor(L, 2);
    if (a && b) {
        PushVxColor(L, *a * *b);
        return 1;
    }
    if (a && lua_isnumber(L, 2)) {
        PushVxColor(L, *a * static_cast<float>(lua_tonumber(L, 2)));
        return 1;
    }
    if (b && lua_isnumber(L, 1)) {
        PushVxColor(L, *b * static_cast<float>(lua_tonumber(L, 1)));
        return 1;
    }
    return luaL_error(L, "VxColor multiplication expects VxColor*VxColor, VxColor*number or number*VxColor");
}

static int VxColorDiv(lua_State *L) {
    auto *a = CheckVxColor(L, 1);
    auto *b = CheckVxColor(L, 2);
    PushVxColor(L, *a / *b);
    return 1;
}

static int VxColorEq(lua_State *L) {
    auto *a = TestVxColor(L, 1);
    auto *b = TestVxColor(L, 2);
    lua_pushboolean(L, a && b && *a == *b);
    return 1;
}

static int VxColorToString(lua_State *L) {
    auto *color = CheckVxColor(L, 1);
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "VxColor(%g, %g, %g, %g)", color->r, color->g, color->b, color->a);
    lua_pushstring(L, buffer);
    return 1;
}

static void SetFunction(lua_State *L, const char *name, lua_CFunction function) {
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, name);
}

static void RegisterClassTable(lua_State *L) {
    lua_newtable(L);
    SetFunction(L, "new", VxColorNew);
    SetFunction(L, "convert", VxColorConvert);

    lua_newtable(L);
    SetFunction(L, "__call", VxColorCall);
    lua_setmetatable(L, -2);

    lua_pushvalue(L, -1);
    lua_setglobal(L, "VxColor");

    lua_getglobal(L, "tas");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "VxColor");
    }
    lua_pop(L, 2);
}

void LuaApi::RegisterVxColor(lua_State *state) {
    tas::lua::LuaStackGuard guard(state);

    tas::lua::LuaUserdataRegistry<VxColor>(state, kVxColorMt)
        .Property<&VxColor::r>("r")
        .Property<&VxColor::g>("g")
        .Property<&VxColor::b>("b")
        .Property<&VxColor::a>("a")
        .Property("rgba", VxColorRGBA, VxColorSetRGBA)
        .ReadonlyProperty("rgb", VxColorRGB)
        .Property("red", VxColorRed, VxColorSetRed)
        .Property("green", VxColorGreen, VxColorSetGreen)
        .Property("blue", VxColorBlue, VxColorSetBlue)
        .Property("alpha", VxColorAlpha, VxColorSetAlpha)
        .Method("clear", VxColorClear)
        .Method("check", VxColorCheck)
        .Method("set", VxColorSet)
        .Method("get_square_distance", VxColorSquareDistance)
        .MetaMethod("__add", VxColorAdd)
        .MetaMethod("__sub", VxColorSub)
        .MetaMethod("__mul", VxColorMul)
        .MetaMethod("__div", VxColorDiv)
        .MetaMethod("__eq", VxColorEq)
        .MetaMethod("__tostring", VxColorToString);

    RegisterClassTable(state);
}
