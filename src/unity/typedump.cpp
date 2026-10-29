// Runtime IL2CPP type dump (our "decompiler" for the game's managed code): for every assembly, every class with its
// parent, fields (offset, type) and methods (RVA in GameAssembly.dll, signature). Written to a text file so the
// analysis can be done offline. Uses only GameAssembly.dll exports; safe to run from an attached non-main thread.
#include "typedump.h"
#include "il2cpp.h"
#include "../common/log.h"
#include <cstdio>
#include <string>

namespace uxr::typedump {
using namespace uxr::il2cpp;

namespace {
struct FieldInfo; struct ParamInfo;
struct X {
    size_t (*image_get_class_count)(const Image*);
    Class* (*image_get_class)(const Image*, size_t);
    const MethodInfo* (*class_get_methods)(Class*, void**);
    FieldInfo* (*class_get_fields)(Class*, void**);
    Class* (*class_get_parent)(Class*);
    const char* (*method_get_name)(const MethodInfo*);
    uint32_t (*method_get_param_count)(const MethodInfo*);
    const Type* (*method_get_param)(const MethodInfo*, uint32_t);
    const char* (*method_get_param_name)(const MethodInfo*, uint32_t);
    const Type* (*method_get_return_type)(const MethodInfo*);
    uint32_t (*method_get_flags)(const MethodInfo*, uint32_t*);
    const char* (*field_get_name)(FieldInfo*);
    size_t (*field_get_offset)(FieldInfo*);
    const Type* (*field_get_type)(FieldInfo*);
    int (*field_get_flags)(FieldInfo*);
    char* (*type_get_name)(const Type*);
    void (*free)(void*);
    bool (*class_is_enum)(const Class*);
    bool (*class_is_valuetype)(const Class*);
    const Class* (*class_get_declaring_type)(Class*);
    Class* (*class_get_interfaces)(Class*, void**);
} x;

template <class T> bool R(T& f, const char* n) {
    f = (T)GetProcAddress(gameAssembly, n);
    if (!f) LOGW("typedump: export %s missing", n);
    return f != nullptr;
}

std::string TypeName(const Type* t) {
    if (!t) return "?";
    char* s = x.type_get_name(t);
    if (!s) return "?";
    std::string r = s;
    if (x.free) x.free(s);
    return r;
}
} // namespace

bool Dump(const wchar_t* path, const char* skipPrefixes) {
    bool ok = true;
    ok &= R(x.image_get_class_count, "il2cpp_image_get_class_count");
    ok &= R(x.image_get_class, "il2cpp_image_get_class");
    ok &= R(x.class_get_methods, "il2cpp_class_get_methods");
    ok &= R(x.class_get_fields, "il2cpp_class_get_fields");
    ok &= R(x.class_get_parent, "il2cpp_class_get_parent");
    ok &= R(x.method_get_name, "il2cpp_method_get_name");
    ok &= R(x.method_get_param_count, "il2cpp_method_get_param_count");
    ok &= R(x.method_get_param, "il2cpp_method_get_param");
    R(x.method_get_param_name, "il2cpp_method_get_param_name");
    ok &= R(x.method_get_return_type, "il2cpp_method_get_return_type");
    R(x.method_get_flags, "il2cpp_method_get_flags");
    ok &= R(x.field_get_name, "il2cpp_field_get_name");
    ok &= R(x.field_get_offset, "il2cpp_field_get_offset");
    ok &= R(x.field_get_type, "il2cpp_field_get_type");
    R(x.field_get_flags, "il2cpp_field_get_flags");
    ok &= R(x.type_get_name, "il2cpp_type_get_name");
    R(x.free, "il2cpp_free");
    R(x.class_is_enum, "il2cpp_class_is_enum");
    R(x.class_is_valuetype, "il2cpp_class_is_valuetype");
    R(x.class_get_declaring_type, "il2cpp_class_get_declaring_type");
    R(x.class_get_interfaces, "il2cpp_class_get_interfaces");
    if (!ok) return false;

    FILE* f = _wfopen(path, L"wb");
    if (!f) { LOGE("typedump: cannot write %ls", path); return false; }
    size_t na = 0;
    const Assembly** as = api.domain_get_assemblies(api.domain_get(), &na);
    size_t total = 0, totalM = 0;
    uintptr_t base = (uintptr_t)gameAssembly;
    for (size_t ai = 0; ai < na; ai++) {
        const Image* img = api.assembly_get_image(as[ai]);
        const char* iname = api.image_get_name(img);
        bool skip = false;
        if (skipPrefixes) {
            std::string sp = skipPrefixes;
            size_t p = 0;
            while (p <= sp.size()) {
                size_t q = sp.find(',', p);
                if (q == std::string::npos) q = sp.size();
                std::string pre = sp.substr(p, q - p);
                if (!pre.empty() && _strnicmp(iname, pre.c_str(), pre.size()) == 0) skip = true;
                p = q + 1;
            }
        }
        size_t nc = x.image_get_class_count(img);
        fprintf(f, "### assembly %s (%zu classes)%s\n", iname, nc, skip ? " [skipped]" : "");
        if (skip) continue;
        for (size_t ci = 0; ci < nc; ci++) {
            Class* k = x.image_get_class(img, ci);
            if (!k) continue;
            total++;
            Class* par = x.class_get_parent(k);
            std::string full = TypeName(api.class_get_type(k));
            const char* kind = (x.class_is_enum && x.class_is_enum(k)) ? "enum"
                             : (x.class_is_valuetype && x.class_is_valuetype(k)) ? "struct" : "class";
            fprintf(f, "\n%s %s", kind, full.c_str());
            if (par) fprintf(f, " : %s", TypeName(api.class_get_type(par)).c_str());
            if (x.class_get_interfaces) {
                void* it = nullptr; bool first = true;
                while (Class* ifc = x.class_get_interfaces(k, &it)) {
                    fprintf(f, "%s%s", first ? (par ? ", " : " : ") : ", ", TypeName(api.class_get_type(ifc)).c_str());
                    first = false;
                }
            }
            fprintf(f, "\n");
            void* it = nullptr;
            while (FieldInfo* fi = x.class_get_fields(k, &it)) {
                int fl = x.field_get_flags ? x.field_get_flags(fi) : 0;
                bool st = (fl & 0x10) != 0, lit = (fl & 0x40) != 0;
                fprintf(f, "  F %s%s%s %s @0x%zx\n", st ? "static " : "", lit ? "const " : "",
                        TypeName(x.field_get_type(fi)).c_str(), x.field_get_name(fi), lit ? 0 : x.field_get_offset(fi));
            }
            it = nullptr;
            while (const MethodInfo* m = x.class_get_methods(k, &it)) {
                void* p = MethodPointer(m);
                uint32_t iflags = 0;
                uint32_t fl = x.method_get_flags ? x.method_get_flags(m, &iflags) : 0;
                std::string sig = TypeName(x.method_get_return_type(m)) + " " + x.method_get_name(m) + "(";
                uint32_t pc = x.method_get_param_count(m);
                for (uint32_t i = 0; i < pc; i++) {
                    if (i) sig += ", ";
                    sig += TypeName(x.method_get_param(m, i));
                    if (x.method_get_param_name) { const char* pn = x.method_get_param_name(m, i); if (pn) { sig += " "; sig += pn; } }
                }
                sig += ")";
                unsigned long long rva = (p && (uintptr_t)p > base && (uintptr_t)p < base + 0x40000000ull) ? (uintptr_t)p - base : 0;
                fprintf(f, "  M %s%s rva=0x%llx\n", (fl & 0x10) ? "static " : "", sig.c_str(), rva);
                totalM++;
            }
        }
    }
    fclose(f);
    LOGI("typedump: %zu assemblies, %zu classes, %zu methods -> %ls", na, total, totalM, path);
    return true;
}

} // namespace uxr::typedump
