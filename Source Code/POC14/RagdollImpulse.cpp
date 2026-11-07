#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "ImpulseModel.h"
#ifndef RAGDOLL_TEST
#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/Character.h>
#include <kenshi/Damages.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/MedicalSystem.h>
#else
#include "tests/runtime_fixture.h"
#endif

namespace
{
    const char* const BUILD_MARKER="ragdoll-impulse-poc14-asset-compensation-experimental";
    const DWORD HIT_VALID_MS=2500;
    const DWORD DEFERRED_MAX_AGE_MS=1500;
    const size_t MAX_DEFERRED_HITS=128;
    const size_t MAX_NATIVE_PARTS=512;
    DWORD g_impulseDelayMs=40;
    HANDLE g_crashTrace=INVALID_HANDLE_VALUE;
    ImpulseModel::Config g_config;
    float g_minDamage=1;
    bool g_enabled=true, g_debug=true, g_ready=false;
    volatile LONG g_suspended=0, g_faulted=0, g_unmatchedCallbacks=0;
    CRITICAL_SECTION g_lock;
    typedef ImpulseModel::HitStore<ImpulseModel::Hit,512> Store;
    Store g_hits;
    typedef void* (*GetRagdollFn)(void*);
    typedef Character* (*GetOwnerFn)(void*);
    typedef void* (*GetActorFn)(void*,const void*);
    GetRagdollFn g_getRagdoll=NULL;
    GetOwnerFn g_getOwner=NULL;
    GetActorFn g_getActor=NULL;
    void* g_purecall[4]={NULL};
    bool g_rotationReady=false;
    volatile LONG g_rotationFaulted=0;
    void** g_rotationActorTable=NULL;
    void* g_rotationMethod=NULL;
    void* g_rotationForceMethod=NULL;
    struct Lock
    {
        Lock() { EnterCriticalSection(&g_lock); }
        ~Lock() { LeaveCriticalSection(&g_lock); }
    };
    void Log(bool error,const char* format,...)
    {
        char body[1024], message[1080];
        va_list args; va_start(args,format);
        _vsnprintf_s(body,sizeof(body),_TRUNCATE,format,args); va_end(args);
        _snprintf_s(message,sizeof(message),_TRUNCATE,"RagdollImpulse: %s",body);
        if (error) ErrorLog(message); else DebugLog(message);
    }
    // Append/flush only this experimental build's actor-call breadcrumbs. A last
    // breadcrumb narrows investigation; it does not prove that call caused a crash.
    void Trace(const char* format,...)
    {
        char body[768], message[896];
        va_list args; va_start(args,format);
        _vsnprintf_s(body,sizeof(body),_TRUNCATE,format,args); va_end(args);
        _snprintf_s(message,sizeof(message),_TRUNCATE,"tick=%lu thread=%lu %s\r\n",GetTickCount(),GetCurrentThreadId(),body);
        if (g_debug) Log(false,"%s",body);
        if (g_crashTrace!=INVALID_HANDLE_VALUE)
        {
            DWORD written=0;
            if (!WriteFile(g_crashTrace,message,static_cast<DWORD>(strlen(message)),&written,NULL) || !FlushFileBuffers(g_crashTrace))
            {
                CloseHandle(g_crashTrace); g_crashTrace=INVALID_HANDLE_VALUE;
                Log(true,"crash_trace_write_failed; continuing with RE_Kenshi logging only");
            }
        }
    }
    bool Active() { return g_ready && !g_suspended && !g_faulted; }
    void Fault(const char* where)
    {
        if (!InterlockedExchange(&g_faulted,1))
            Log(true,"physics_disabled_for_session reason=%s; no further custom actor calls",where);
    }
    float ReadFloat(const char* path,const char* key,float fallback,float lo,float hi)
    {
        char value[96], def[40]; sprintf_s(def,sizeof(def),"%.6g",fallback);
        GetPrivateProfileStringA("RagdollImpulse",key,def,value,sizeof(value),path);
        char* end=NULL; const double number=strtod(value,&end);
        while (end && (*end==' ' || *end=='\t')) ++end;
        if (end==value || !end || *end || !_finite(number) || number<lo || number>hi)
        { Log(true,"invalid_config key=%s value=%s fallback=%.3f",key,value,fallback); return fallback; }
        return static_cast<float>(number);
    }
    void LoadConfig()
    {
        char path[MAX_PATH]="RagdollImpulse_Config.ini";
        HMODULE module=NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LoadConfig),&module))
        {
            char full[MAX_PATH]; const DWORD n=GetModuleFileNameA(module,full,MAX_PATH);
            if (n && n<MAX_PATH)
            {
                char* slash=strrchr(full,'\\');
                if (slash) { slash[1]=0; _snprintf_s(path,sizeof(path),_TRUNCATE,"%sRagdollImpulse_Config.ini",full); }
            }
        }
        g_enabled=GetPrivateProfileIntA("RagdollImpulse","Enabled",1,path)!=0;
        g_debug=GetPrivateProfileIntA("RagdollImpulse","DebugLogging",1,path)!=0;
        g_minDamage=ReadFloat(path,"MinDamage",1,0,10000);
        g_config.base=ReadFloat(path,"BaseImpulse",g_config.base,0,100000);
        g_config.damageScale=ReadFloat(path,"DamageScale",g_config.damageScale,0,1000);
        g_config.maxImpulse=ReadFloat(path,"MaxImpulse",g_config.maxImpulse,0,100000);
        g_config.upward=ReadFloat(path,"UpwardImpulse",g_config.upward,0,1000);
        g_config.swing=ReadFloat(path,"SwingDirectionCoefficient",g_config.swing,0,2);
        g_config.maxVertical=ReadFloat(path,"MaxDirectionalVerticalImpulse",g_config.maxVertical,0,10000);
        g_config.spread=ReadFloat(path,"ImpactSpread",g_config.spread,0,1);
        g_config.maxSpeedChange=ReadFloat(path,"MaxActorSpeedChange",g_config.maxSpeedChange,0,100000);
        g_config.rotationCoefficient=ReadFloat(path,"RotationalImpulseCoefficient",g_config.rotationCoefficient,0,100);
        g_config.maxRotation=ReadFloat(path,"MaxRotationalImpulse",g_config.maxRotation,0,100000);
        g_config.huMRagdollMultiplier=ReadFloat(path,"HuMRagdollImpulseMultiplier",g_config.huMRagdollMultiplier,1,100);
        g_impulseDelayMs=static_cast<DWORD>(ReadFloat(path,"ImpulseDelayMs",40,1,500));
        if (g_enabled && GetPrivateProfileIntA("RagdollImpulse","CrashTrace",1,path))
        {
            char tracePath[MAX_PATH]; strcpy_s(tracePath,sizeof(tracePath),path);
            char* slash=strrchr(tracePath,'\\');
            if (slash) slash[1]=0; else tracePath[0]=0;
            strcat_s(tracePath,sizeof(tracePath),"RagdollImpulse_CrashTrace.log");
            g_crashTrace=CreateFileA(tracePath,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
            if (g_crashTrace==INVALID_HANDLE_VALUE) Log(true,"crash_trace_open_failed path=%s error=%lu",tracePath,GetLastError());
            Trace("session_start build=%s pid=%lu",BUILD_MARKER,GetCurrentProcessId());
        }
        Log(false,"build_marker=%s enabled=%d base=%.2f scale=%.2f cap=%.2f upward=%.2f swing=%.3f vertical_cap=%.2f spread=%.3f speed_change_cap=%.2f delay_ms=%lu config=%s",
            BUILD_MARKER,g_enabled?1:0,g_config.base,g_config.damageScale,g_config.maxImpulse,
            g_config.upward,g_config.swing,g_config.maxVertical,g_config.spread,g_config.maxSpeedChange,g_impulseDelayMs,path);
        Log(false,"EXPERIMENTAL delayed_cached_actors=1; lifetime/threading safety unproven; use a disposable save");
        Log(false,"rotation_config coefficient=%.3f cap=%.2f contact=estimated_attacker_facing_side targets=hit_region_only",g_config.rotationCoefficient,g_config.maxRotation);
        Log(false,"asset_compensation file=hu_m_ragdoll.phs multiplier=%.3f order=after_linear_caps rotation=unchanged speed_limiter=final",g_config.huMRagdollMultiplier);
    }
    void* Resolve(const char* name)
    {
        HMODULE lib=GetModuleHandleA("KenshiLib.dll");
        void* fn=lib?reinterpret_cast<void*>(GetProcAddress(lib,name)):NULL;
        if (!fn) { Log(true,"missing_export %s",name); return NULL; }
        return reinterpret_cast<void*>(KenshiLib::GetRealAddress(fn));
    }
    bool Hook(void* address,void* hook,void** original,const char* label)
    {
        if (!address || KenshiLib::SUCCESS!=KenshiLib::AddHook(address,hook,original) || !*original)
        { Log(true,"hook_failed %s",label); return false; }
        Log(false,"hook_installed %s",label); return true;
    }
    bool Executable(const void* address)
    {
        MEMORY_BASIC_INFORMATION info;
        if (!address || !VirtualQuery(address,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_GUARD|PAGE_NOACCESS))) return false;
        DWORD p=info.Protect&0xff;
        return p==PAGE_EXECUTE || p==PAGE_EXECUTE_READ || p==PAGE_EXECUTE_READWRITE || p==PAGE_EXECUTE_WRITECOPY;
    }
    bool ActorMethod(void* fn)
    {
        if (!Executable(fn)) return false;
        for (size_t i=0;i<4;++i) if (g_purecall[i] && fn==g_purecall[i]) return false;
        return true;
    }
    // Layout binding for the inspected Steam 1.0.65 binary. See the adapter notes.
    bool ValidateNativeAdapter(void* callback)
    {
        __try
        {
            unsigned char* base=reinterpret_cast<unsigned char*>(GetModuleHandleA(NULL));
            IMAGE_DOS_HEADER* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (!base || dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
            IMAGE_NT_HEADERS64* pe=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
            if (pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
                pe->FileHeader.TimeDateStamp!=0x65d604d7 || pe->OptionalHeader.SizeOfImage!=0x232c000) return false;
            const unsigned char callbackBytes[]={0x48,0x8b,0xc4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa1};
            const unsigned char lookupBytes[]={0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x83,0xec,0x20,0x48};
            const unsigned char ragdollBytes[]={0x48,0x8b,0x81,0xe8,0x02,0,0,0xc3};
            const unsigned char ownerBytes[]={0x48,0x8b,0x81,0xd8,0x02,0,0,0xc3};
            // RagdollClass+0 -> Scythe entity; entity+8 -> retained PhysFileParams.
            // Params begins with the file string; its +0x68 points back to ragdoll.
            // See diagnostics/poc14_asset_adapter.md for construction/lifetime evidence.
            const unsigned char entityStore[]={0x48,0x89,0x01};
            const unsigned char paramsStore[]={0x48,0x89,0x51,0x08};
            const unsigned char backrefStore[]={0x4c,0x89,0x5e,0x68};
            if (callback!=base+0x5b7c90 || reinterpret_cast<void*>(g_getRagdoll)!=base+0x5c4b00 ||
                reinterpret_cast<void*>(g_getOwner)!=base+0x5c4b10 ||
                memcmp(callback,callbackBytes,sizeof(callbackBytes)) || memcmp(base+0x1b1e50,lookupBytes,sizeof(lookupBytes)) ||
                memcmp(base+0x5c4b00,ragdollBytes,sizeof(ragdollBytes)) || memcmp(base+0x5c4b10,ownerBytes,sizeof(ownerBytes)) ||
                memcmp(base+0x5b9293,entityStore,sizeof(entityStore)) || memcmp(base+0x7e9834,paramsStore,sizeof(paramsStore)) ||
                memcmp(base+0x5b9252,backrefStore,sizeof(backrefStore))) return false;
            g_getActor=reinterpret_cast<GetActorFn>(base+0x1b1e50);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    // Verified in the installed PhysX 2.8.4 DLL, not inferred from a modern SDK:
    // vtable +0x250 (slot 74) -> NpActor::addTorque, world-space vector/mode/wake.
    // See diagnostics/poc13_rotation_adapter.md for binary and direction evidence.
    bool ValidateRotationAdapter()
    {
        __try
        {
            unsigned char* base=reinterpret_cast<unsigned char*>(GetModuleHandleA("PhysXCore64.dll"));
            if (!base) return false;
            IMAGE_DOS_HEADER* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic!=IMAGE_DOS_SIGNATURE) return false;
            IMAGE_NT_HEADERS64* pe=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
            if (pe->Signature!=IMAGE_NT_SIGNATURE || pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
                pe->FileHeader.TimeDateStamp!=0x4e13e0c6 || pe->OptionalHeader.SizeOfImage!=0x45b000) return false;
            const unsigned char prefix[]={0x40,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x98,0,0,0,0x48};
            if (memcmp(base+0x177120,prefix,sizeof(prefix)) || memcmp(base+0x177340,prefix,sizeof(prefix))) return false;
            void** table=reinterpret_cast<void**>(base+0x3b3750);
            if (table[72]!=base+0x177340 || table[74]!=base+0x177120 || !ActorMethod(table[74])) return false;
            g_rotationActorTable=table; g_rotationMethod=table[74]; g_rotationForceMethod=table[72];
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool RotationActive() { return g_rotationReady && !g_rotationFaulted; }
    void RotationFault(const char* reason)
    {
        if (!InterlockedExchange(&g_rotationFaulted,1))
            Log(true,"rotation_disabled_for_session reason=%s; existing linear impulse path retained",reason);
    }
    struct NativeBodies
    {
        void* actors[ImpulseModel::MaxActors];
        void** tables[ImpulseModel::MaxActors];
        void* forceMethods[ImpulseModel::MaxActors];
        void* torqueMethods[ImpulseModel::MaxActors];
        ImpulseModel::Body body[ImpulseModel::MaxActors];
        char asset[MAX_PATH];
        size_t count, parts, inactive, kinematic, missing;
        NativeBodies() : count(0),parts(0),inactive(0),kinematic(0),missing(0)
        { asset[0]=0; memset(actors,0,sizeof(actors)); memset(tables,0,sizeof(tables)); memset(forceMethods,0,sizeof(forceMethods)); memset(torqueMethods,0,sizeof(torqueMethods)); }
    };
    struct DeferredHit
    {
        bool valid;
        uintptr_t owner, animation;
        uint64_t serial;
        DWORD capturedAt, captureThread;
        NativeBodies native;
        ImpulseModel::Result result;
        DeferredHit() : valid(false),owner(0),animation(0),serial(0),capturedAt(0),captureThread(0) {}
    };
    DeferredHit g_deferred[MAX_DEFERRED_HITS];
    volatile LONG g_draining=0;
    // All queue metadata access holds g_lock. Cancellation never dereferences a
    // cached owner, animation or actor. It cannot stop a native call already begun.
    void CancelDeferred(uintptr_t owner,uintptr_t animation,const char* reason)
    {
        Lock lock;
        for (size_t i=0;i<MAX_DEFERRED_HITS;++i)
            if (g_deferred[i].valid && g_deferred[i].animation==animation && (!owner || g_deferred[i].owner==owner))
            { Log(false,"deferred_cancelled hit=%I64u reason=%s",g_deferred[i].serial,reason); g_deferred[i].valid=false; }
    }
    void ClearDeferred()
    {
        Lock lock;
        for (size_t i=0;i<MAX_DEFERRED_HITS;++i) g_deferred[i].valid=false;
    }
    bool CopyNativeName(const unsigned char* str,char* out,size_t capacity)
    {
        const size_t length=*reinterpret_cast<const size_t*>(str+0x10);
        const size_t allocated=*reinterpret_cast<const size_t*>(str+0x18);
        if (!length || length>=capacity || length>allocated || allocated>65536) return false;
        const char* name=allocated<16?reinterpret_cast<const char*>(str):*reinterpret_cast<const char* const*>(str);
        if (!name || name[length]!=0) return false;
        memcpy(out,name,length); out[length]=0; return true;
    }
    bool CopyRagdollAsset(const unsigned char* rag,char* out,size_t capacity)
    {
        out[0]=0;
        __try
        {
            const unsigned char* entity=*reinterpret_cast<const unsigned char* const*>(rag);
            if (!entity) return false;
            const unsigned char* params=*reinterpret_cast<const unsigned char* const*>(entity+8);
            if (!params || *reinterpret_cast<const void* const*>(params+0x68)!=rag ||
                !CopyNativeName(params,out,capacity)) { out[0]=0; return false; }
            const size_t length=*reinterpret_cast<const size_t*>(params+0x10);
            // Reject embedded terminators/control characters, not partial names.
            for (size_t i=0;i<length;++i)
                if (static_cast<unsigned char>(out[i])<32) { out[0]=0; return false; }
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { out[0]=0; return false; }
    }
    bool CollectNativeBodies(void* animation,NativeBodies& out)
    {
        typedef bool (*IsDynamicFn)(void*);
        typedef float (*MassFn)(void*);
        __try
        {
            unsigned char* rag=static_cast<unsigned char*>(g_getRagdoll(animation));
            if (!rag) return true;
            void* skeleton=*reinterpret_cast<void**>(rag+8);
            if (!skeleton) return true;
            // Read once at native setup; the deferred event owns a value copy.
            // Unknown/unreadable asset => no compensation, retain existing force.
            CopyRagdollAsset(rag,out.asset,sizeof(out.asset));
            const unsigned int count=*reinterpret_cast<unsigned int*>(rag+0x30);
            unsigned char** parts=*reinterpret_cast<unsigned char***>(rag+0x38);
            if (count>MAX_NATIVE_PARTS || (count && !parts)) return false;
            out.parts=count;
            for (unsigned int i=0;i<count;++i)
            {
                unsigned char* part=parts[i];
                if (!part || part[0x30]>1 || part[0x31]>1) return false;
                if (!part[0x30]) { ++out.inactive; continue; }
                if (part[0x31]) { ++out.kinematic; continue; }
                void* actor=g_getActor(skeleton,part);
                if (!actor) { ++out.missing; continue; }
                bool duplicate=false;
                for (size_t j=0;j<out.count;++j) if (out.actors[j]==actor) duplicate=true;
                if (duplicate) continue;
                if (out.count==ImpulseModel::MaxActors) return false;
                void** table=*reinterpret_cast<void***>(actor);
                if (!table || !ActorMethod(table[29]) || !ActorMethod(table[46]) || !ActorMethod(table[72])) return false;
                if (!reinterpret_cast<IsDynamicFn>(table[29])(actor)) { ++out.inactive; continue; }
                const float mass=reinterpret_cast<MassFn>(table[46])(actor);
                if (!ImpulseModel::Finite(mass) || mass<=0 || mass>1000000) return false;
                if (!CopyNativeName(part,out.body[out.count].bone,sizeof(out.body[out.count].bone))) return false;
                out.actors[out.count]=actor; out.tables[out.count]=table; out.forceMethods[out.count]=table[72];
                if (RotationActive() && table==g_rotationActorTable && table[72]==g_rotationForceMethod &&
                    table[74]==g_rotationMethod && ActorMethod(table[74])) out.torqueMethods[out.count]=table[74];
                out.body[out.count].mass=mass; ++out.count;
            }
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ApplyDeferredForce(const DeferredHit& event,size_t index)
    {
        typedef void (*ForceFn)(void*,const ImpulseModel::Vec&,int,bool);
        void* actor=event.native.actors[index];
        __try
        {
            void** table=*reinterpret_cast<void***>(actor);
            if (!table || table!=event.native.tables[index] || table[72]!=event.native.forceMethods[index] || !ActorMethod(table[72]))
            {
                Trace("deferred_actor_rejected hit=%I64u actor=%p reason=vtable_or_force_method_changed",event.serial,actor);
                return false;
            }
            Trace("deferred_force_enter hit=%I64u actor=%p method=%p",event.serial,actor,table[72]);
            reinterpret_cast<ForceFn>(table[72])(actor,event.result.impulse[index],1,true); // NX_IMPULSE, wake
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        { Trace("deferred_actor_exception hit=%I64u actor=%p code=0x%08lx",event.serial,actor,GetExceptionCode()); return false; }
    }
    bool ApplyDeferredRotation(const DeferredHit& event,size_t index)
    {
        typedef void (*TorqueFn)(void*,const ImpulseModel::Vec&,int,bool);
        void* actor=event.native.actors[index];
        __try
        {
            void** table=*reinterpret_cast<void***>(actor);
            if (!table || table!=event.native.tables[index] || table!=g_rotationActorTable ||
                !event.native.torqueMethods[index] || table[74]!=event.native.torqueMethods[index] ||
                table[74]!=g_rotationMethod || !ActorMethod(table[74]))
            {
                Trace("deferred_torque_rejected hit=%I64u actor=%p reason=vtable_or_torque_method_changed",event.serial,actor);
                return false;
            }
            Trace("deferred_torque_enter hit=%I64u actor=%p method=%p",event.serial,actor,table[74]);
            reinterpret_cast<TorqueFn>(table[74])(actor,event.result.rotationalImpulse[index],1,true); // NX_IMPULSE, world space, wake
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER)
        { Trace("deferred_torque_exception hit=%I64u actor=%p code=0x%08lx",event.serial,actor,GetExceptionCode()); return false; }
    }
    bool ReadOwner(void* animation,Character*& owner)
    {
        __try { owner=g_getOwner(animation); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    const char* MatchName(ImpulseModel::Result::Match match)
    {
        return match==ImpulseModel::Result::ExactBone?"exact_bone":
            (match==ImpulseModel::Result::BodyArea?"body_area":"whole_body_fallback");
    }
    void QueueAfterSetup(void* animation)
    {
        Character* owner=NULL;
        if (!ReadOwner(animation,owner)) { Fault("owner_getter"); return; }
        ImpulseModel::Hit hit; uint64_t serial=0; bool found;
        { Lock lock; found=g_hits.Take(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),GetTickCount(),HIT_VALID_MS,hit,serial); }
        if (!found)
        {
            if (g_debug && InterlockedIncrement(&g_unmatchedCallbacks)<=8)
                Log(false,"postsetup_callback owner=%p animation=%p pending_hit=0 thread=%lu",owner,animation,GetCurrentThreadId());
            return;
        }
        NativeBodies native;
        if (!CollectNativeBodies(animation,native)) { Fault("native_body_validation"); return; }
        if (!native.count)
        {
            Log(true,"postsetup_skipped hit=%I64u owner=%p reason=no_dynamic_bodies parts=%Iu inactive=%Iu kinematic=%Iu missing=%Iu",
                serial,owner,native.parts,native.inactive,native.kinematic,native.missing);
            return;
        }
        ImpulseModel::Result result;
        if (!ImpulseModel::Build(g_config,hit,native.body,native.count,result,native.asset)) { Fault("impulse_math"); return; }
        result.plannedRotation=0;
        for (size_t i=0;i<native.count;++i)
        {
            if (result.rotationalImpulse[i].length()>.000001f && !native.torqueMethods[i])
            {
                Log(false,"rotation_skipped hit=%I64u actor=%p bone=\"%s\" reason=unsupported_or_disabled_torque_adapter",serial,native.actors[i],native.body[i].bone);
                result.rotationalImpulse[i]=ImpulseModel::Vec();
            }
            result.plannedRotation+=result.rotationalImpulse[i].length();
        }
        bool queued=false;
        {
            Lock lock;
            if (!Active()) return;
            const DWORD now=GetTickCount();
            for (size_t i=0;i<MAX_DEFERRED_HITS;++i)
            {
                DeferredHit& event=g_deferred[i];
                if (event.valid && static_cast<DWORD>(now-event.capturedAt)>DEFERRED_MAX_AGE_MS)
                { Log(false,"deferred_expired hit=%I64u reason=queue_age",event.serial); event.valid=false; }
                if (event.valid) continue;
                event.owner=reinterpret_cast<uintptr_t>(owner); event.animation=reinterpret_cast<uintptr_t>(animation);
                event.serial=serial; event.capturedAt=now; event.captureThread=GetCurrentThreadId();
                event.native=native; event.result=result; event.valid=true; queued=true; break;
            }
        }
        if (!queued) { Log(true,"deferred_skipped hit=%I64u reason=queue_full capacity=%Iu",serial,MAX_DEFERRED_HITS); return; }
        Log(false,"deferred_queued hit=%I64u owner=%p animation=%p actors=%Iu parts=%Iu kinematic=%Iu bone=\"%s\" match=%s targets=%Iu damage=%.2f requested_horizontal=%.2f planned_horizontal=%.2f safety_scale=%.5f requested_rotation=%.2f planned_rotation=%.2f thread=%lu asset=\"%s\" asset_multiplier=%.3f",
            serial,owner,animation,native.count,native.parts,native.kinematic,hit.bone,MatchName(result.match),result.targets,hit.damage,
            result.requestedHorizontal,result.deliveredHorizontal,result.scale,result.requestedRotation,result.plannedRotation,GetCurrentThreadId(),
            native.asset[0]?native.asset:"unknown",result.assetMultiplier);
    }
    void DrainDeferred()
    {
        if (!Active() || InterlockedCompareExchange(&g_draining,1,0)) return;
        struct DrainScope { ~DrainScope() { InterlockedExchange(&g_draining,0); } } drainScope;
        for (size_t slot=0;slot<MAX_DEFERRED_HITS && Active();++slot)
        {
            DeferredHit event;
            {
                Lock lock;
                DeferredHit& pending=g_deferred[slot];
                if (!pending.valid) continue;
                const DWORD age=static_cast<DWORD>(GetTickCount()-pending.capturedAt);
                if (age>DEFERRED_MAX_AGE_MS)
                { Log(false,"deferred_expired hit=%I64u age_ms=%lu",pending.serial,age); pending.valid=false; continue; }
                if (age<g_impulseDelayMs) continue;
                event=pending; pending.valid=false; // Consume before any external actor call.
            }
            Trace("deferred_apply_begin hit=%I64u owner=%p actors=%Iu age_ms=%lu capture_thread=%lu planned_horizontal=%.2f safety_scale=%.5f",
                event.serial,reinterpret_cast<void*>(event.owner),event.native.count,static_cast<DWORD>(GetTickCount()-event.capturedAt),
                event.captureThread,event.result.deliveredHorizontal,event.result.scale);
            for (size_t i=0;i<event.native.count;++i)
            {
                if (!Active()) return;
                const ImpulseModel::Vec& impulse=event.result.impulse[i];
                if (impulse.length()<.000001f) continue;
                Trace("deferred_actor_begin hit=%I64u actor=%p bone=\"%s\" mass=%.5f impulse=(%.3f,%.3f,%.3f) expected_vtable=%p",
                    event.serial,event.native.actors[i],event.native.body[i].bone,event.native.body[i].mass,
                    impulse.x,impulse.y,impulse.z,event.native.tables[i]);
                if (!ApplyDeferredForce(event,i)) { Fault("deferred_actor_validation_or_force"); ClearDeferred(); return; }
                Trace("deferred_actor_returned hit=%I64u actor=%p call_returned=1",event.serial,event.native.actors[i]);
                const ImpulseModel::Vec& rotation=event.result.rotationalImpulse[i];
                if (RotationActive() && Active() && rotation.length()>.000001f)
                {
                    Trace("deferred_torque_begin hit=%I64u actor=%p bone=\"%s\" angular_impulse=(%.3f,%.3f,%.3f)",
                        event.serial,event.native.actors[i],event.native.body[i].bone,rotation.x,rotation.y,rotation.z);
                    if (!ApplyDeferredRotation(event,i)) RotationFault("deferred_torque_validation_or_call");
                    else Trace("deferred_torque_returned hit=%I64u actor=%p call_returned=1",event.serial,event.native.actors[i]);
                }
            }
            Trace("deferred_apply_end hit=%I64u",event.serial);
        }
    }
    void ExpireHits(DWORD now)
    {
        Lock lock;
        now=GetTickCount(); // Sample after the lock: a concurrent record may be newer than the caller's tick.
        for (size_t i=0;i<512;++i)
        {
            Store::Entry& e=g_hits.entries[i];
            if (e.valid && static_cast<DWORD>(now-e.at)>HIT_VALID_MS)
            {
                if (e.pending) Log(true,"pending_hit_expired hit=%I64u owner=%p reason=no_postsetup_callback",
                    e.serial,reinterpret_cast<void*>(e.owner));
                e=Store::Entry();
            }
        }
    }
    ImpulseModel::Cut ConvertCut(CutDirection c)
    {
        if (c==CUT_LEFT || c==CUT_REAR_LEFT) return ImpulseModel::Left;
        if (c==CUT_RIGHT || c==CUT_REAR_RIGHT) return ImpulseModel::Right;
        if (c==CUT_UPWARDS) return ImpulseModel::Up;
        if (c==CUT_DOWNWARD || c==CUT_REAR_DOWNWARD) return ImpulseModel::Down;
        return ImpulseModel::Straight;
    }
    struct WoundContext
    {
        Character* victim; RootObject* attacker; ImpulseModel::Hit hit; bool wound;
        WoundContext() : victim(NULL),attacker(NULL),wound(false) {}
    };
    __declspec(thread) WoundContext* g_currentMelee=NULL;
    struct MeleeScope
    {
        WoundContext* previous;
        MeleeScope(WoundContext* context) : previous(g_currentMelee) { g_currentMelee=context; }
        ~MeleeScope() { g_currentMelee=previous; }
    };
}
HitMaterialType (*g_originalHit)(Character*,CutDirection,Damages&,Character*,CombatTechniqueData*,int)=NULL;
GameData* (*g_originalWound)(MedicalSystem*,bool,CutDirection,Damages&,int&,RootObject*,AttackDirection::Enum&,Harpoon*)=NULL;
bool (*g_originalRagdoll)(Character*,const Character::RagdollMsg&)=NULL;
void (*g_originalSetup)(void*,bool)=NULL;
void (*g_originalLoop)(GameWorld*,float)=NULL;

HitMaterialType HitHook(Character* victim,CutDirection cut,Damages& damage,Character* attacker,CombatTechniqueData* attack,int combo)
{
    WoundContext context; context.victim=victim; context.attacker=attacker; context.hit.cut=ConvertCut(cut);
    if (Active() && victim && attacker)
    {
        const Ogre::Vector3 delta=victim->getPosition()-attacker->getPosition();
        context.hit.direction=ImpulseModel::Vec(delta.x,0,delta.z);
    }
    MeleeScope scope(&context);
    const HitMaterialType material=g_originalHit(victim,cut,damage,attacker,attack,combo);
    if (Active() && victim && attacker && context.wound)
    {
        context.hit.damage=damage.total();
        const float horizontalLength=context.hit.direction.length();
        if (!ImpulseModel::Finite(horizontalLength) || horizontalLength<.0001f)
        {
            if (g_debug) Log(false,"melee_skipped owner=%p reason=unavailable_hit_direction",victim);
            return material;
        }
        if (ImpulseModel::Finite(context.hit.damage) && context.hit.damage>=g_minDamage && context.hit.damage>0)
        {
            void* animation=victim->getAnimationClass(); bool recorded=false;
            if (animation)
            { Lock lock; recorded=g_hits.Record(reinterpret_cast<uintptr_t>(victim),reinterpret_cast<uintptr_t>(animation),context.hit,GetTickCount(),HIT_VALID_MS); }
            if (g_debug) Log(false,"melee_final owner=%p animation=%p damage=%.2f cut=%d bone=\"%s\" recorded=%d",
                victim,animation,context.hit.damage,static_cast<int>(cut),context.hit.bone,recorded?1:0);
        }
    }
    return material;
}
GameData* WoundHook(MedicalSystem* self,bool low,CutDirection area,Damages& damage,int& material,RootObject* attacker,AttackDirection::Enum& direction,Harpoon* harpoon)
{
    GameData* part=g_originalWound(self,low,area,damage,material,attacker,direction,harpoon);
    WoundContext* context=g_currentMelee;
    if (Active() && context && self && part && context->victim==self->me && context->attacker==attacker)
    {
        const std::string& bone=MedicalSystem::getBoneNameForBodypart(part);
        strncpy_s(context->hit.bone,sizeof(context->hit.bone),bone.c_str(),_TRUNCATE);
        context->hit.area=ImpulseModel::Classify(part->name.c_str());
        if (context->hit.area==ImpulseModel::Unknown) context->hit.area=ImpulseModel::Classify(context->hit.bone);
        context->hit.side=ImpulseModel::Side(part->name.c_str());
        if (!context->hit.side) context->hit.side=ImpulseModel::Side(context->hit.bone);
        context->wound=true;
    }
    return part;
}
bool RagdollHook(Character* owner,const Character::RagdollMsg& message)
{
    void* animation=NULL; uint64_t serial=0;
    if (Active() && owner)
    {
        animation=owner->getAnimationClass(); Lock lock;
        CancelDeferred(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),message.on?"new_transition":"ragdoll_off");
        if (message.on) serial=g_hits.Arm(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),GetTickCount(),HIT_VALID_MS);
        else g_hits.Cancel(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),0);
    }
    if (serial && g_debug) Log(false,"ragdoll_armed hit=%I64u owner=%p animation=%p thread=%lu",serial,owner,animation,GetCurrentThreadId());
    const bool result=g_originalRagdoll(owner,message);
    if (serial && !result)
    {
        Lock lock; g_hits.Cancel(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),serial);
        CancelDeferred(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),"transition_failed");
        if (g_debug) Log(false,"ragdoll_cancelled hit=%I64u reason=transition_failed",serial);
    }
    return result;
}
void SetupHook(void* animation,bool flag)
{
    if (g_ready && animation) CancelDeferred(0,reinterpret_cast<uintptr_t>(animation),"native_setup_restarted");
    g_originalSetup(animation,flag); // Native activation and zero-velocity initialization first.
    if (Active() && animation) QueueAfterSetup(animation); // Actual force is deferred to a later main-loop tick.
}
void LoopHook(GameWorld* world,float time)
{
    if (g_ready && world && world->isLoadingFromASaveGame())
    { InterlockedExchange(&g_suspended,1); Lock lock; g_hits.Clear(); ClearDeferred(); }
    g_originalLoop(world,time);
    if (g_ready && world)
    {
        if (world->isLoadingFromASaveGame())
        { InterlockedExchange(&g_suspended,1); Lock lock; g_hits.Clear(); ClearDeferred(); }
        else
        {
            InterlockedExchange(&g_suspended,0); ExpireHits(GetTickCount());
            if (!world->isPaused()) DrainDeferred();
        }
    }
}
__declspec(dllexport) void startPlugin()
{
    InitializeCriticalSection(&g_lock); LoadConfig();
    if (!g_enabled) { Log(false,"disabled_by_config no_hooks=1"); return; }
    void* hit=Resolve("?_NV_hitByMeleeAttack@Character@@QEAA?AW4HitMaterialType@@W4CutDirection@@AEAVDamages@@PEAV1@PEAVCombatTechniqueData@@H@Z");
    void* wound=Resolve("?addWound@MedicalSystem@@QEAAPEAVGameData@@_NW4CutDirection@@AEAVDamages@@AEAHPEAVRootObject@@AEAW4Enum@AttackDirection@@PEAVHarpoon@@@Z");
    void* ragdoll=Resolve("?_ragdollMode@Character@@QEAA_NAEBURagdollMsg@1@@Z");
    void* setup=Resolve("?_NV_postRagdollCreationCallbackMT@AnimationClass@@QEAAX_N@Z");
    void* loop=Resolve("?_NV_mainLoop_GPUSensitiveStuff@GameWorld@@QEAAXM@Z");
    g_getRagdoll=reinterpret_cast<GetRagdollFn>(Resolve("?_NV_getRagdoll@AnimationClass@@QEBAPEAVRagdollClass@@XZ"));
    g_getOwner=reinterpret_cast<GetOwnerFn>(Resolve("?_NV_getCharacter@AnimationClass@@QEAAPEAVCharacter@@XZ"));
    if (!hit || !wound || !ragdoll || !setup || !loop || !g_getRagdoll || !g_getOwner || !ValidateNativeAdapter(setup))
    { Log(true,"disabled unsupported_native_layout_or_missing_export; requires inspected Steam 1.0.65; no_hooks=1"); return; }
    const char* crt[]={"msvcr100.dll","msvcr90.dll","msvcrt.dll","ucrtbase.dll"};
    for (size_t i=0;i<4;++i)
    { HMODULE module=GetModuleHandleA(crt[i]); if (module) g_purecall[i]=reinterpret_cast<void*>(GetProcAddress(module,"_purecall")); }
    if (g_config.rotationCoefficient>0 && g_config.maxRotation>0)
    {
        g_rotationReady=ValidateRotationAdapter();
        Log(!g_rotationReady,"rotation_adapter ready=%d table=%p torque=%p; linear path unchanged if unsupported",g_rotationReady?1:0,g_rotationActorTable,g_rotationMethod);
    }
    else Log(false,"rotation_disabled_by_config; linear path unchanged");
    const bool a=Hook(hit,&HitHook,reinterpret_cast<void**>(&g_originalHit),"final_melee");
    const bool b=Hook(wound,&WoundHook,reinterpret_cast<void**>(&g_originalWound),"hit_bodypart");
    const bool c=Hook(ragdoll,&RagdollHook,reinterpret_cast<void**>(&g_originalRagdoll),"ragdoll_transition");
    const bool d=Hook(setup,&SetupHook,reinterpret_cast<void**>(&g_originalSetup),"post_ragdoll_setup");
    const bool e=Hook(loop,&LoopHook,reinterpret_cast<void**>(&g_originalLoop),"load_and_expiry");
    g_ready=a && b && c && d && e;
    Log(!g_ready,"ready=%d actor_calls=delayed_main_loop owner_scoped=1 actor_cache=experimental reactions=physical_localization teleport=never max_pending_characters=512 max_deferred_hits=%Iu",g_ready?1:0,MAX_DEFERRED_HITS);
}
