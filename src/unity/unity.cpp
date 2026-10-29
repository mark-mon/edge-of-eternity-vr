#include "unity.h"
#include "../common/log.h"
#include <unordered_map>
#include <cstring>
#include <cstdio>

namespace uxr::unity {

Methods M{};
Icalls I{};
Class* GameObjectClass = nullptr;
static const MethodInfo* (*p_class_get_methods)(Class*, void**) = nullptr;
static const char* (*p_method_get_name)(const MethodInfo*) = nullptr;
static uint32_t (*p_method_get_param_count)(const MethodInfo*) = nullptr;
static Class* (*p_class_get_parent)(Class*) = nullptr;
static void* (*p_class_get_field_from_name)(Class*, const char*) = nullptr;
static size_t (*p_field_get_offset)(void*) = nullptr;

#define ICALL(field, name)                                                        \
    I.field = (decltype(I.field))api.resolve_icall(name);                         \
    if (!I.field) { LOGE("unity: icall %s missing", name); ok = false; }

static bool InitIcalls() {
    bool ok = true;
    ICALL(Camera_Render, "UnityEngine.Camera::Render()")
    ICALL(Camera_set_worldToCameraMatrix, "UnityEngine.Camera::set_worldToCameraMatrix_Injected(UnityEngine.Matrix4x4&)")
    ICALL(Camera_ResetWorldToCameraMatrix, "UnityEngine.Camera::ResetWorldToCameraMatrix()")
    ICALL(Camera_set_projectionMatrix, "UnityEngine.Camera::set_projectionMatrix_Injected(UnityEngine.Matrix4x4&)")
    ICALL(Camera_ResetProjectionMatrix, "UnityEngine.Camera::ResetProjectionMatrix()")
    ICALL(Camera_set_targetTexture, "UnityEngine.Camera::set_targetTexture(UnityEngine.RenderTexture)")
    ICALL(Camera_get_targetTexture, "UnityEngine.Camera::get_targetTexture()")
    ICALL(Camera_get_nearClipPlane, "UnityEngine.Camera::get_nearClipPlane()")
    ICALL(Camera_get_farClipPlane, "UnityEngine.Camera::get_farClipPlane()")
    ICALL(Transform_get_position, "UnityEngine.Transform::get_position_Injected(UnityEngine.Vector3&)")
    ICALL(Transform_get_rotation, "UnityEngine.Transform::get_rotation_Injected(UnityEngine.Quaternion&)")
    ICALL(GL_Clear, "UnityEngine.GL::GLClear_Injected(System.Boolean,System.Boolean,UnityEngine.Color&,System.Single)")
    ICALL(GL_IssuePluginEvent, "UnityEngine.GL::GLIssuePluginEvent(System.IntPtr,System.Int32)")
    ICALL(RenderTexture_GetActive, "UnityEngine.RenderTexture::GetActive()")
    ICALL(RenderTexture_SetActive, "UnityEngine.RenderTexture::SetActive(UnityEngine.RenderTexture)")
    ICALL(RenderTexture_Create, "UnityEngine.RenderTexture::Create()")
    ICALL(RenderTexture_Release, "UnityEngine.RenderTexture::Release()")
    ICALL(Texture_GetNativeTexturePtr, "UnityEngine.Texture::GetNativeTexturePtr()")
    ICALL(Object_set_hideFlags, "UnityEngine.Object::set_hideFlags(UnityEngine.HideFlags)")
    ICALL(Graphics_Blit4, "UnityEngine.Graphics::Blit4_Injected(UnityEngine.Texture,UnityEngine.RenderTexture,UnityEngine.Vector2&,UnityEngine.Vector2&)")
    ICALL(Behaviour_set_enabled, "UnityEngine.Behaviour::set_enabled(System.Boolean)")
    ICALL(Behaviour_get_enabled, "UnityEngine.Behaviour::get_enabled()")
    return ok;
}

Object* NewRenderTexture(int w, int h, int depthBits, int format, const char* debugName) {
    Object* rt = api.object_new(M.RenderTexture);
    if (!rt) return nullptr;
    if (M.RenderTexture_ctor4) {
        void* args[] = {&w, &h, &depthBits, &format};
        Exception* ex = nullptr;
        api.runtime_invoke(M.RenderTexture_ctor4, rt, args, &ex);
        if (ex) { LOGE("unity: RenderTexture ctor threw"); return nullptr; }
    } else if (M.RenderTexture_ctor3) {
        void* args[] = {&w, &h, &depthBits};
        Exception* ex = nullptr;
        api.runtime_invoke(M.RenderTexture_ctor3, rt, args, &ex);
        if (ex) { LOGE("unity: RenderTexture ctor threw"); return nullptr; }
    } else { LOGE("unity: no RenderTexture constructor"); return nullptr; }
    I.Object_set_hideFlags(rt, 32 /* DontUnloadUnusedAsset */ | 20 /* DontSave */);
    if (!I.RenderTexture_Create(rt)) LOGW("unity: RenderTexture.Create(%s) returned false", debugName);
    LOGI("unity: created RenderTexture '%s' %dx%d depth %d fmt %d native %p", debugName, w, h, depthBits, format,
         I.Texture_GetNativeTexturePtr(rt));
    return rt;
}

bool Init() {
    const char* core = "UnityEngine.CoreModule.dll";
    M.Object = FindClass("UnityEngine", "Object", core);
    M.Component = FindClass("UnityEngine", "Component", core);
    M.Behaviour = FindClass("UnityEngine", "Behaviour", core);
    M.Camera = FindClass("UnityEngine", "Camera", core);
    M.Transform = FindClass("UnityEngine", "Transform", core);
    M.Texture = FindClass("UnityEngine", "Texture", core);
    M.RenderTexture = FindClass("UnityEngine", "RenderTexture", core);
    M.SystemInfo = FindClass("UnityEngine", "SystemInfo", core);
    M.Screen = FindClass("UnityEngine", "Screen", core);
    M.Time = FindClass("UnityEngine", "Time", core);
    M.Application = FindClass("UnityEngine", "Application", core);
    M.SceneManager = FindClass("UnityEngine.SceneManagement", "SceneManager", core);
    M.GL = FindClass("UnityEngine", "GL", core);
    M.QualitySettings = FindClass("UnityEngine", "QualitySettings", core);
    M.Canvas = FindClass("UnityEngine", "Canvas", "UnityEngine.UIModule.dll");
    GameObjectClass = FindClass("UnityEngine", "GameObject", core);
    p_class_get_methods = (decltype(p_class_get_methods))GetProcAddress(gameAssembly, "il2cpp_class_get_methods");
    p_method_get_name = (decltype(p_method_get_name))GetProcAddress(gameAssembly, "il2cpp_method_get_name");
    p_method_get_param_count = (decltype(p_method_get_param_count))GetProcAddress(gameAssembly, "il2cpp_method_get_param_count");
    p_class_get_parent = (decltype(p_class_get_parent))GetProcAddress(gameAssembly, "il2cpp_class_get_parent");
    p_class_get_field_from_name = (decltype(p_class_get_field_from_name))GetProcAddress(gameAssembly, "il2cpp_class_get_field_from_name");
    p_field_get_offset = (decltype(p_field_get_offset))GetProcAddress(gameAssembly, "il2cpp_field_get_offset");
    if (!M.Object || !M.Camera || !M.Transform || !GameObjectClass) return false;

    M.Object_get_name = FindMethod(M.Object, "get_name", 0);
    M.Object_FindObjectsOfType = FindMethod(M.Object, "FindObjectsOfType", 1);
    M.Component_get_transform = FindMethod(M.Component, "get_transform", 0);
    M.Component_get_gameObject = FindMethod(M.Component, "get_gameObject", 0);
    M.Behaviour_get_enabled = FindMethod(M.Behaviour, "get_enabled", 0);
    M.Behaviour_get_isActiveAndEnabled = FindMethod(M.Behaviour, "get_isActiveAndEnabled", 0);
    M.Camera_get_depth = FindMethod(M.Camera, "get_depth", 0);
    M.Camera_get_cullingMask = FindMethod(M.Camera, "get_cullingMask", 0);
    M.Camera_get_clearFlags = FindMethod(M.Camera, "get_clearFlags", 0);
    M.Camera_get_fieldOfView = FindMethod(M.Camera, "get_fieldOfView", 0);
    M.Camera_get_orthographic = FindMethod(M.Camera, "get_orthographic", 0);
    M.Camera_get_pixelRect = FindMethod(M.Camera, "get_pixelRect", 0);
    M.Camera_get_targetTexture = FindMethod(M.Camera, "get_targetTexture", 0);
    M.Camera_get_nearClipPlane = FindMethod(M.Camera, "get_nearClipPlane", 0);
    M.Camera_get_farClipPlane = FindMethod(M.Camera, "get_farClipPlane", 0);
    M.Camera_FireOnPreCull = FindMethod(M.Camera, "FireOnPreCull", 1);
    M.Camera_FireOnPostRender = FindMethod(M.Camera, "FireOnPostRender", 1);
    M.Transform_get_position = FindMethod(M.Transform, "get_position", 0);
    M.Transform_get_rotation = FindMethod(M.Transform, "get_rotation", 0);
    M.Texture_get_width = FindMethod(M.Texture, "get_width", 0);
    M.Texture_get_height = FindMethod(M.Texture, "get_height", 0);
    if (M.Canvas) {
        M.Canvas_get_renderMode = FindMethod(M.Canvas, "get_renderMode", 0);
        M.Canvas_get_worldCamera = FindMethod(M.Canvas, "get_worldCamera", 0);
        M.Canvas_get_sortingOrder = FindMethod(M.Canvas, "get_sortingOrder", 0);
        M.Canvas_get_isRootCanvas = FindMethod(M.Canvas, "get_isRootCanvas", 0);
        M.Canvas_SendWillRenderCanvases = FindMethod(M.Canvas, "SendWillRenderCanvases", 0);
    }
    if (M.Application) {
        M.Application_InvokeOnBeforeRender = FindMethod(M.Application, "InvokeOnBeforeRender", 0);
    }
    M.SystemInfo_get_graphicsDeviceType = FindMethod(M.SystemInfo, "get_graphicsDeviceType", 0);
    M.Screen_get_width = FindMethod(M.Screen, "get_width", 0);
    M.Screen_get_height = FindMethod(M.Screen, "get_height", 0);
    M.SceneManager_GetActiveScene = FindMethod(M.SceneManager, "GetActiveScene", 0);
    M.RenderTexture_ctor4 = api.class_get_method_from_name(M.RenderTexture, ".ctor", 4);
    M.RenderTexture_ctor3 = api.class_get_method_from_name(M.RenderTexture, ".ctor", 3);
    M.QualitySettings_get_activeColorSpace = FindMethod(M.QualitySettings, "get_activeColorSpace", 0);
    LOGI("unity: RenderTexture ctor(4)=%p ctor(3)=%p", M.RenderTexture_ctor4, M.RenderTexture_ctor3);
    if (!InitIcalls()) return false;
    M.Time_get_frameCount = (int (*)())api.resolve_icall("UnityEngine.Time::get_frameCount()");
    M.Scene_GetNameInternal =
        (String * (*)(int)) api.resolve_icall("UnityEngine.SceneManagement.Scene::GetNameInternal(System.Int32)");
    LOGI("unity: icalls frameCount=%p sceneName=%p", M.Time_get_frameCount, M.Scene_GetNameInternal);
    return true;
}


const MethodInfo* Method(Class* k, const char* name, int argc) {
    static std::unordered_map<std::string, const MethodInfo*> cache;
    if (!k) return nullptr;
    char key[256];
    snprintf(key, sizeof(key), "%p/%s/%d", (void*)k, name, argc);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const MethodInfo* m = nullptr;
    for (Class* c = k; c && !m; c = p_class_get_parent ? p_class_get_parent(c) : nullptr)
        m = api.class_get_method_from_name(c, name, argc);
    if (!m) LOGW("unity: method %s.%s(%d) not found", api.class_get_name(k), name, argc);
    cache[key] = m;
    return m;
}

Object* Call(Class* k, const char* name, void* self, std::initializer_list<void*> args) {
    const MethodInfo* m = Method(k, name, (int)args.size());
    if (!m) return nullptr;
    void* a[8] = {};
    int i = 0;
    for (void* p : args) if (i < 8) a[i++] = p;
    return Invoke(m, self, a);
}

Class* ClassOf(Object* o) { return o ? api.object_get_class(o) : nullptr; }

std::string ClassName(Object* o) {
    Class* k = ClassOf(o);
    if (!k) return "<null>";
    std::string ns = api.class_get_namespace(k);
    return ns.empty() ? std::string(api.class_get_name(k)) : ns + "." + api.class_get_name(k);
}

Object* GameObjectOf(Object* c) {
    if (!c) return nullptr;
    if (ClassOf(c) == GameObjectClass) return c;
    return Invoke(M.Component_get_gameObject, c);
}

std::vector<Object*> Components(Object* goOrComp) {
    std::vector<Object*> out;
    Object* go = GameObjectOf(goOrComp);
    if (!go) return out;
    Array* a = (Array*)Call(GameObjectClass, "GetComponents", go, {TypeObject(M.Component)});
    if (!a) return out;
    uint32_t n = api.array_length(a);
    Object** d = ArrayData(a);
    out.assign(d, d + n);
    return out;
}

Object* FindComponent(Object* goOrComp, const char* cls) {
    for (Object* c : Components(goOrComp))
        if (c && strcmp(api.class_get_name(ClassOf(c)), cls) == 0) return c;
    return nullptr;
}

int FieldOffset(Class* k, const char* field) {
    for (Class* c = k; c; c = p_class_get_parent ? p_class_get_parent(c) : nullptr) {
        void* f = p_class_get_field_from_name ? p_class_get_field_from_name(c, field) : nullptr;
        if (f) return (int)p_field_get_offset(f);
    }
    return -1;
}

bool GetStatic(Class* k, const char* field, void* out) {
    static auto get = (void (*)(void*, void*))GetProcAddress(gameAssembly, "il2cpp_field_static_get_value");
    void* f = (k && p_class_get_field_from_name) ? p_class_get_field_from_name(k, field) : nullptr;
    if (!f || !get) return false;
    get(f, out);
    return true;
}
bool SetStatic(Class* k, const char* field, void* value) {
    static auto set = (void (*)(void*, void*))GetProcAddress(gameAssembly, "il2cpp_field_static_set_value");
    void* f = (k && p_class_get_field_from_name) ? p_class_get_field_from_name(k, field) : nullptr;
    if (!f || !set) return false;
    set(f, value);
    return true;
}

Object* NewGameObject(const char* name) {
    Object* go = api.object_new(GameObjectClass);
    if (!go) return nullptr;
    const MethodInfo* ctor = api.class_get_method_from_name(GameObjectClass, ".ctor", 1);
    void* a[] = {api.string_new(name)};
    Exception* ex = nullptr;
    api.runtime_invoke(ctor, go, a, &ex);
    if (ex) { LOGE("unity: GameObject ctor threw"); return nullptr; }
    Call(M.Object, "DontDestroyOnLoad", nullptr, {go});
    return go;
}

Object* AddComponent(Object* go, Class* k) { return Call(GameObjectClass, "AddComponent", go, {TypeObject(k)}); }

std::string Name(Object* o) {
    if (!o) return "<null>";
    return ToUtf8((String*)Invoke(M.Object_get_name, o));
}

std::vector<Object*> FindObjectsOfType(Class* k) {
    std::vector<Object*> out;
    void* args[] = {TypeObject(k)};
    Array* a = (Array*)Invoke(M.Object_FindObjectsOfType, nullptr, args);
    if (!a) return out;
    uint32_t n = api.array_length(a);
    Object** d = ArrayData(a);
    out.assign(d, d + n);
    return out;
}

std::string ActiveSceneName() {
    int handle = 0;
    if (!M.Scene_GetNameInternal || !InvokeValue(M.SceneManager_GetActiveScene, nullptr, handle)) return "?";
    return ToUtf8(M.Scene_GetNameInternal(handle));
}

int FrameCount() { return M.Time_get_frameCount ? M.Time_get_frameCount() : -1; }

const char* GraphicsDeviceTypeName(int t) {
    switch (t) {
    case 2: return "Direct3D11";
    case 4: return "Null";
    case 17: return "OpenGLCore";
    case 18: return "Direct3D12";
    case 21: return "Vulkan";
    default: return "other";
    }
}

} // namespace uxr::unity
