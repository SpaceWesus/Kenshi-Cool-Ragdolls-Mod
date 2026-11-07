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
    const char* const BUILD_MARKER="ragdoll-impulse-poc11-postsetup-localized";
    const DWORD HIT_VALID_MS=2500;
    const size_t MAX_NATIVE_PARTS=512;
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
        g_config.maxSpeedChange=ReadFloat(path,"MaxActorSpeedChange",g_config.maxSpeedChange,.1f,200);
        Log(false,"build_marker=%s enabled=%d base=%.2f scale=%.2f cap=%.2f upward=%.2f swing=%.3f vertical_cap=%.2f spread=%.3f speed_change_cap=%.2f config=%s",
            BUILD_MARKER,g_enabled?1:0,g_config.base,g_config.damageScale,g_config.maxImpulse,
            g_config.upward,g_config.swing,g_config.maxVertical,g_config.spread,g_config.maxSpeedChange,path);
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
            if (callback!=base+0x5b7c90 || reinterpret_cast<void*>(g_getRagdoll)!=base+0x5c4b00 ||
                reinterpret_cast<void*>(g_getOwner)!=base+0x5c4b10 ||
                memcmp(callback,callbackBytes,sizeof(callbackBytes)) || memcmp(base+0x1b1e50,lookupBytes,sizeof(lookupBytes)) ||
                memcmp(base+0x5c4b00,ragdollBytes,sizeof(ragdollBytes)) || memcmp(base+0x5c4b10,ownerBytes,sizeof(ownerBytes))) return false;
            g_getActor=reinterpret_cast<GetActorFn>(base+0x1b1e50);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    struct NativeBodies
    {
        void* actors[ImpulseModel::MaxActors];
        ImpulseModel::Body body[ImpulseModel::MaxActors];
        size_t count, parts, inactive, kinematic, missing;
        NativeBodies() : count(0),parts(0),inactive(0),kinematic(0),missing(0) {}
    };
    bool CopyNativeName(const unsigned char* str,char* out,size_t capacity)
    {
        const size_t length=*reinterpret_cast<const size_t*>(str+0x10);
        const size_t allocated=*reinterpret_cast<const size_t*>(str+0x18);
        if (!length || length>=capacity || length>allocated || allocated>65536) return false;
        const char* name=allocated<16?reinterpret_cast<const char*>(str):*reinterpret_cast<const char* const*>(str);
        if (!name || name[length]!=0) return false;
        memcpy(out,name,length); out[length]=0; return true;
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
                out.actors[out.count]=actor; out.body[out.count].mass=mass; ++out.count;
            }
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    bool ApplyForce(void* actor,const ImpulseModel::Vec& impulse)
    {
        typedef void (*ForceFn)(void*,const ImpulseModel::Vec&,int,bool);
        __try
        {
            void** table=*reinterpret_cast<void***>(actor);
            if (!table || !ActorMethod(table[72])) return false;
            reinterpret_cast<ForceFn>(table[72])(actor,impulse,1,true); // NX_IMPULSE, wake
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
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
    void ApplyAfterSetup(void* animation)
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
        if (!ImpulseModel::Build(g_config,hit,native.body,native.count,result)) { Fault("impulse_math"); return; }
        Log(false,"postsetup_impulse hit=%I64u owner=%p animation=%p actors=%Iu parts=%Iu kinematic=%Iu bone=\"%s\" match=%s targets=%Iu damage=%.2f requested_horizontal=%.2f planned_horizontal=%.2f safety_scale=%.5f thread=%lu",
            serial,owner,animation,native.count,native.parts,native.kinematic,hit.bone,MatchName(result.match),result.targets,hit.damage,
            result.requestedHorizontal,result.deliveredHorizontal,result.scale,GetCurrentThreadId());
        for (size_t i=0;i<native.count;++i)
        {
            const ImpulseModel::Vec& impulse=result.impulse[i];
            if (impulse.length()<.000001f) continue;
            if (!ApplyForce(native.actors[i],impulse)) { Fault("add_force"); return; }
            if (g_debug) Log(false,"actor_impulse_postsetup hit=%I64u owner=%p actor=%p bone=\"%s\" mass=%.5f impulse=(%.3f,%.3f,%.3f) call_returned=1",
                serial,owner,native.actors[i],native.body[i].bone,native.body[i].mass,impulse.x,impulse.y,impulse.z);
        }
        // Actor pointers leave scope before returning to the native caller.
    }
    void ExpireHits(DWORD now)
    {
        Lock lock;
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
        if (message.on) serial=g_hits.Arm(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),GetTickCount(),HIT_VALID_MS);
        else g_hits.Cancel(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),0);
    }
    if (serial && g_debug) Log(false,"ragdoll_armed hit=%I64u owner=%p animation=%p thread=%lu",serial,owner,animation,GetCurrentThreadId());
    const bool result=g_originalRagdoll(owner,message);
    if (serial && !result)
    {
        Lock lock; g_hits.Cancel(reinterpret_cast<uintptr_t>(owner),reinterpret_cast<uintptr_t>(animation),serial);
        if (g_debug) Log(false,"ragdoll_cancelled hit=%I64u reason=transition_failed",serial);
    }
    return result;
}
void SetupHook(void* animation,bool flag)
{
    g_originalSetup(animation,flag); // Native activation and zero-velocity initialization first.
    if (Active() && animation) ApplyAfterSetup(animation);
}
void LoopHook(GameWorld* world,float time)
{
    if (g_ready && world && world->isLoadingFromASaveGame())
    { InterlockedExchange(&g_suspended,1); Lock lock; g_hits.Clear(); }
    g_originalLoop(world,time);
    if (g_ready && world)
    {
        if (world->isLoadingFromASaveGame())
        { InterlockedExchange(&g_suspended,1); Lock lock; g_hits.Clear(); }
        else { InterlockedExchange(&g_suspended,0); ExpireHits(GetTickCount()); }
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
    const bool a=Hook(hit,&HitHook,reinterpret_cast<void**>(&g_originalHit),"final_melee");
    const bool b=Hook(wound,&WoundHook,reinterpret_cast<void**>(&g_originalWound),"hit_bodypart");
    const bool c=Hook(ragdoll,&RagdollHook,reinterpret_cast<void**>(&g_originalRagdoll),"ragdoll_transition");
    const bool d=Hook(setup,&SetupHook,reinterpret_cast<void**>(&g_originalSetup),"post_ragdoll_setup");
    const bool e=Hook(loop,&LoopHook,reinterpret_cast<void**>(&g_originalLoop),"load_and_expiry");
    g_ready=a && b && c && d && e;
    Log(!g_ready,"ready=%d actor_calls=post_native_setup owner_scoped=1 actor_cache=none reactions=physical_localization teleport=never max_pending_characters=512",g_ready?1:0);
}
