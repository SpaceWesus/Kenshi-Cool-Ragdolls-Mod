#include <Debug.h>
#include <core/Functions.h>
#include <kenshi/Character.h>
#include <kenshi/Damages.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/PhysicsActual.h>

#include <ogre/OgreVector3.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace
{
    const char* const BUILD_MARKER = "ragdoll-impulse-poc10-immediate-safe";
    const DWORD HIT_VALID_MS = 2500;
    const size_t NXACTOR_VTBL_ADD_FORCE = 72;
    const int NX_FORCE_MODE_IMPULSE = 1;

    struct ImpulseVector
    {
        float x;
        float y;
        float z;

        ImpulseVector() : x(0.0f), y(0.0f), z(0.0f) {}
        ImpulseVector(float xValue, float yValue, float zValue)
            : x(xValue), y(yValue), z(zValue) {}
    };

    float g_minDamage = 1.0f;
    float g_baseImpulse = 700.0f;
    float g_damageScale = 35.0f;
    float g_maxImpulse = 3500.0f;
    float g_upwardImpulse = 6.0f;
    float g_swingDirectionCoefficient = 0.15f;
    float g_maxDirectionalVerticalImpulse = 1050.0f;
    float g_angularImpulseCoefficient = 0.18f;
    float g_maxAngularImpulse = 450.0f;
    float g_reactionVariation = 0.08f;
    float g_headSpinMultiplier = 1.45f;
    float g_torsoSpinMultiplier = 0.75f;
    float g_armSpinMultiplier = 1.10f;
    float g_legSpinMultiplier = 1.20f;
    float g_headLiftMultiplier = 1.15f;
    float g_torsoLiftMultiplier = 1.00f;
    float g_armLiftMultiplier = 0.90f;
    float g_legLiftMultiplier = 0.25f;
    float g_legHorizontalMultiplier = 0.70f;
    size_t g_expectedActorCount = 13;
    DWORD g_captureGraceMs = 1500;

    struct RecentHit
    {
        Character* victim;
        Character* attacker;
        Ogre::Vector3 direction;
        CutDirection cutDirection;
        GameData* bodyPart;
        char boneName[96];
        float damage;
        DWORD timeMs;
        bool valid;
    };

    bool g_enabled = true;
    bool g_debugLogging = true;
    bool g_ready = false;
    CRITICAL_SECTION g_stateLock;
    Character* g_captureOwner = NULL;
    DWORD g_captureStartMs = 0;
    DWORD g_lastActorMs = 0;
    DWORD g_captureThreadId = 0;
    struct RecentWound
    {
        Character* victim;
        RootObject* attacker;
        GameData* bodyPart;
        char boneName[96];
        DWORD timeMs;
        bool valid;
    };

    RecentHit g_hit = {};
    RecentHit g_captureHit = {};
    RecentWound g_wound = {};
    std::vector<NxActor*> g_actors;

    void LogMessage(bool error, const char* format, ...)
    {
        char body[768];
        va_list args;
        va_start(args, format);
        vsprintf_s(body, sizeof(body), format, args);
        va_end(args);

        char message[832];
        sprintf_s(message, sizeof(message), "RagdollImpulse: %s", body);
        if (error)
            ErrorLog(message);
        else
            DebugLog(message);
    }

    float ClampFloat(float value, float minimum, float maximum)
    {
        if (value < minimum)
            return minimum;
        if (value > maximum)
            return maximum;
        return value;
    }

    float ReadConfigFloat(const char* path, const char* key, float fallback)
    {
        char fallbackText[32];
        char valueText[64];
        sprintf_s(fallbackText, sizeof(fallbackText), "%.3f", fallback);
        GetPrivateProfileStringA("RagdollImpulse", key, fallbackText,
            valueText, sizeof(valueText), path);
        char* end = NULL;
        const double parsed = strtod(valueText, &end);
        return end != valueText ? static_cast<float>(parsed) : fallback;
    }

    void LoadConfig()
    {
        char path[MAX_PATH] = "RagdollImpulse_Config.ini";
        HMODULE module = NULL;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&LoadConfig), &module))
        {
            char modulePath[MAX_PATH];
            const DWORD length = GetModuleFileNameA(module, modulePath, MAX_PATH);
            if (length > 0 && length < MAX_PATH)
            {
                char* slash = strrchr(modulePath, '\\');
                if (slash)
                {
                    *(slash + 1) = '\0';
                    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sRagdollImpulse_Config.ini", modulePath);
                }
            }
        }

        g_enabled = GetPrivateProfileIntA("RagdollImpulse", "Enabled", 1, path) != 0;
        g_debugLogging = GetPrivateProfileIntA("RagdollImpulse", "DebugLogging", 1, path) != 0;
        g_minDamage = ClampFloat(ReadConfigFloat(path, "MinDamage", g_minDamage), 0.0f, 10000.0f);
        g_baseImpulse = ClampFloat(ReadConfigFloat(path, "BaseImpulse", g_baseImpulse), 0.0f, 100000.0f);
        g_damageScale = ClampFloat(ReadConfigFloat(path, "DamageScale", g_damageScale), 0.0f, 1000.0f);
        g_maxImpulse = ClampFloat(ReadConfigFloat(path, "MaxImpulse", g_maxImpulse), 0.0f, 100000.0f);
        g_upwardImpulse = ClampFloat(ReadConfigFloat(path, "UpwardImpulse", g_upwardImpulse), 0.0f, 50.0f);
        g_swingDirectionCoefficient = ClampFloat(ReadConfigFloat(
            path, "SwingDirectionCoefficient", g_swingDirectionCoefficient), 0.0f, 2.0f);
        g_maxDirectionalVerticalImpulse = ClampFloat(ReadConfigFloat(
            path, "MaxDirectionalVerticalImpulse", g_maxDirectionalVerticalImpulse), 0.0f, 10000.0f);
        g_angularImpulseCoefficient = ClampFloat(ReadConfigFloat(
            path, "AngularImpulseCoefficient", g_angularImpulseCoefficient), 0.0f, 1.0f);
        g_maxAngularImpulse = ClampFloat(ReadConfigFloat(
            path, "MaxAngularImpulse", g_maxAngularImpulse), 0.0f, 5000.0f);
        g_reactionVariation = ClampFloat(ReadConfigFloat(
            path, "ReactionVariation", g_reactionVariation), 0.0f, 0.25f);
        g_headSpinMultiplier = ClampFloat(ReadConfigFloat(path, "HeadSpinMultiplier", g_headSpinMultiplier), 0.0f, 3.0f);
        g_torsoSpinMultiplier = ClampFloat(ReadConfigFloat(path, "TorsoSpinMultiplier", g_torsoSpinMultiplier), 0.0f, 3.0f);
        g_armSpinMultiplier = ClampFloat(ReadConfigFloat(path, "ArmSpinMultiplier", g_armSpinMultiplier), 0.0f, 3.0f);
        g_legSpinMultiplier = ClampFloat(ReadConfigFloat(path, "LegSpinMultiplier", g_legSpinMultiplier), 0.0f, 3.0f);
        g_headLiftMultiplier = ClampFloat(ReadConfigFloat(path, "HeadLiftMultiplier", g_headLiftMultiplier), 0.0f, 3.0f);
        g_torsoLiftMultiplier = ClampFloat(ReadConfigFloat(path, "TorsoLiftMultiplier", g_torsoLiftMultiplier), 0.0f, 3.0f);
        g_armLiftMultiplier = ClampFloat(ReadConfigFloat(path, "ArmLiftMultiplier", g_armLiftMultiplier), 0.0f, 3.0f);
        g_legLiftMultiplier = ClampFloat(ReadConfigFloat(path, "LegLiftMultiplier", g_legLiftMultiplier), 0.0f, 3.0f);
        g_legHorizontalMultiplier = ClampFloat(ReadConfigFloat(path,
            "LegHorizontalMultiplier", g_legHorizontalMultiplier), 0.0f, 2.0f);
        g_expectedActorCount = static_cast<size_t>(GetPrivateProfileIntA(
            "RagdollImpulse", "ExpectedActorCount", static_cast<int>(g_expectedActorCount), path));
        g_captureGraceMs = static_cast<DWORD>(GetPrivateProfileIntA(
            "RagdollImpulse", "CaptureGraceMs", static_cast<int>(g_captureGraceMs), path));
        if (g_expectedActorCount < 2 || g_expectedActorCount > 64)
            g_expectedActorCount = 13;
        if (g_captureGraceMs < 250 || g_captureGraceMs > 5000)
            g_captureGraceMs = 1500;
        if (g_maxImpulse < g_baseImpulse)
            g_baseImpulse = g_maxImpulse;

        LogMessage(false,
            "build_marker=%s enabled=%d debug=%d min_damage=%.2f base=%.2f scale=%.2f cap=%.2f upward=%.2f swing_coeff=%.3f vertical_cap=%.2f angular_coeff=%.3f angular_cap=%.2f variation=%.3f expected_actors=%Iu grace_ms=%lu",
            BUILD_MARKER, g_enabled ? 1 : 0, g_debugLogging ? 1 : 0,
            g_minDamage, g_baseImpulse, g_damageScale, g_maxImpulse, g_upwardImpulse,
            g_swingDirectionCoefficient, g_maxDirectionalVerticalImpulse,
            g_angularImpulseCoefficient, g_maxAngularImpulse, g_reactionVariation,
            g_expectedActorCount,
            static_cast<unsigned long>(g_captureGraceMs));
    }

    void* FindRealExport(const char* exportName, const char* label)
    {
        const HMODULE library = GetModuleHandleA("KenshiLib.dll");
        void* exported = library ? reinterpret_cast<void*>(GetProcAddress(library, exportName)) : NULL;
        if (!exported)
        {
            LogMessage(true, "missing KenshiLib export for %s", label);
            return NULL;
        }

        void* address = reinterpret_cast<void*>(KenshiLib::GetRealAddress(exported));
        if (!address)
            LogMessage(true, "null real address for %s", label);
        return address;
    }

    bool InstallHook(void* address, void* hook, void** original, const char* label)
    {
        if (KenshiLib::SUCCESS != KenshiLib::AddHook(address, hook, original) || !*original)
        {
            LogMessage(true, "hook failed for %s", label);
            return false;
        }
        if (g_debugLogging)
            LogMessage(false, "hook installed for %s", label);
        return true;
    }

    Ogre::Vector3 HorizontalDirection(Character* victim, Character* attacker)
    {
        Ogre::Vector3 direction = victim->getPosition() - attacker->getPosition();
        direction.y = 0.0f;
        const float lengthSq = direction.x * direction.x + direction.z * direction.z;
        if (lengthSq < 0.0001f)
            return Ogre::Vector3(0.0f, 0.0f, 1.0f);
        const float inverseLength = 1.0f / sqrtf(lengthSq);
        return Ogre::Vector3(direction.x * inverseLength, 0.0f, direction.z * inverseLength);
    }

    const char* CutDirectionName(CutDirection direction)
    {
        switch (direction)
        {
        case CUT_DOWNWARD: return "downward";
        case CUT_LEFT: return "left";
        case CUT_RIGHT: return "right";
        case CUT_THRUST: return "thrust";
        case CUT_UPWARDS: return "upwards";
        case CUT_PIERCED: return "pierced";
        case CUT_REAR_DOWNWARD: return "rear_downward";
        case CUT_REAR_LEFT: return "rear_left";
        case CUT_REAR_RIGHT: return "rear_right";
        default: return "default";
        }
    }

    float SwingVerticalFactor(CutDirection direction)
    {
        if (direction == CUT_DOWNWARD || direction == CUT_REAR_DOWNWARD)
            return -1.0f;
        if (direction == CUT_UPWARDS)
            return 1.0f;
        return 0.0f;
    }

    float SwingLateralFactor(CutDirection direction)
    {
        if (direction == CUT_LEFT || direction == CUT_REAR_LEFT)
            return -1.0f;
        if (direction == CUT_RIGHT || direction == CUT_REAR_RIGHT)
            return 1.0f;
        return 0.0f;
    }

    bool ContainsIgnoreCase(const char* text, const char* needle)
    {
        if (!text || !needle || !*needle)
            return false;
        const size_t needleLength = strlen(needle);
        for (const char* cursor = text; *cursor; ++cursor)
        {
            if (_strnicmp(cursor, needle, needleLength) == 0)
                return true;
        }
        return false;
    }

    enum ReactionArea
    {
        REACTION_HEAD,
        REACTION_TORSO,
        REACTION_ARM,
        REACTION_LEG
    };

    ReactionArea ClassifyReactionArea(const RecentHit& hit)
    {
        const char* body = hit.bodyPart ? hit.bodyPart->name.c_str() : "";
        const char* bone = hit.boneName;
        if (ContainsIgnoreCase(body, "head") || ContainsIgnoreCase(bone, "head") ||
            ContainsIgnoreCase(body, "neck") || ContainsIgnoreCase(bone, "neck"))
            return REACTION_HEAD;
        if (ContainsIgnoreCase(body, "arm") || ContainsIgnoreCase(bone, "arm") ||
            ContainsIgnoreCase(body, "hand") || ContainsIgnoreCase(bone, "hand"))
            return REACTION_ARM;
        if (ContainsIgnoreCase(body, "leg") || ContainsIgnoreCase(bone, "thigh") ||
            ContainsIgnoreCase(bone, "calf") || ContainsIgnoreCase(body, "foot") ||
            ContainsIgnoreCase(bone, "foot"))
            return REACTION_LEG;
        return REACTION_TORSO;
    }

    const char* ReactionAreaName(ReactionArea area)
    {
        switch (area)
        {
        case REACTION_HEAD: return "head";
        case REACTION_ARM: return "arm";
        case REACTION_LEG: return "leg";
        default: return "torso";
        }
    }

    float SpinMultiplier(ReactionArea area)
    {
        if (area == REACTION_HEAD) return g_headSpinMultiplier;
        if (area == REACTION_ARM) return g_armSpinMultiplier;
        if (area == REACTION_LEG) return g_legSpinMultiplier;
        return g_torsoSpinMultiplier;
    }

    float LiftMultiplier(ReactionArea area)
    {
        if (area == REACTION_HEAD) return g_headLiftMultiplier;
        if (area == REACTION_ARM) return g_armLiftMultiplier;
        if (area == REACTION_LEG) return g_legLiftMultiplier;
        return g_torsoLiftMultiplier;
    }

    float HorizontalMultiplier(ReactionArea area)
    {
        return area == REACTION_LEG ? g_legHorizontalMultiplier : 1.0f;
    }

    float DeterministicSignedVariation(const RecentHit& hit)
    {
        ULONG_PTR value = reinterpret_cast<ULONG_PTR>(hit.victim);
        value ^= reinterpret_cast<ULONG_PTR>(hit.attacker) >> 4;
        value ^= static_cast<ULONG_PTR>(hit.timeMs) * 2654435761UL;
        value ^= value >> 13;
        value *= 2246822519UL;
        const unsigned int sample = static_cast<unsigned int>(value & 0xFFFF);
        return static_cast<float>(sample) / 32767.5f - 1.0f;
    }

    bool TryGetRecentHit(Character* victim, RecentHit* out)
    {
        EnterCriticalSection(&g_stateLock);
        const bool matches = g_hit.valid && g_hit.victim == victim &&
            static_cast<DWORD>(GetTickCount() - g_hit.timeMs) <= HIT_VALID_MS;
        if (matches && out)
            *out = g_hit;
        LeaveCriticalSection(&g_stateLock);
        return matches;
    }

    bool CaptureIsCurrent()
    {
        return g_captureOwner &&
            static_cast<DWORD>(GetTickCount() - g_captureStartMs) <= g_captureGraceMs;
    }

    bool ApplyImpulseGuarded(NxActor* actor, const ImpulseVector& impulse);

    bool BeginCapture(Character* owner)
    {
        RecentHit hit;
        const bool recentMelee = TryGetRecentHit(owner, &hit);
        if (!recentMelee || hit.damage < g_minDamage)
        {
            if (g_debugLogging)
                LogMessage(false,
                    "ragdoll_transition phase=ignored character=%p recent_melee=%d damage=%.2f",
                    owner, recentMelee ? 1 : 0, recentMelee ? hit.damage : 0.0f);
            return false;
        }

        const DWORD now = GetTickCount();
        const DWORD threadId = GetCurrentThreadId();
        EnterCriticalSection(&g_stateLock);
        if (g_captureOwner &&
            static_cast<DWORD>(now - g_captureStartMs) <= g_captureGraceMs)
        {
            Character* activeOwner = g_captureOwner;
            LeaveCriticalSection(&g_stateLock);
            if (g_debugLogging)
                LogMessage(false,
                    "ragdoll_transition phase=ignored_busy character=%p active=%p damage=%.2f",
                    owner, activeOwner, hit.damage);
            return false;
        }
        g_captureOwner = owner;
        g_captureStartMs = now;
        g_lastActorMs = 0;
        g_captureThreadId = threadId;
        g_captureHit = hit;
        g_actors.clear();
        LeaveCriticalSection(&g_stateLock);
        if (g_debugLogging)
            LogMessage(false,
                "ragdoll_transition phase=begin character=%p recent_melee=1 damage=%.2f thread=%lu",
                owner, hit.damage, static_cast<unsigned long>(threadId));
        return true;
    }

    void CaptureActor(NxActor* actor, const char* source, NxScene* scene)
    {
        if (!g_ready || !actor)
            return;

        RecentHit hit = {};
        Character* owner = NULL;
        size_t actorIndex = 0;
        size_t actorCount = 0;
        bool captureComplete = false;
        EnterCriticalSection(&g_stateLock);
        if (!CaptureIsCurrent())
        {
            LeaveCriticalSection(&g_stateLock);
            return;
        }
        for (std::vector<NxActor*>::const_iterator it = g_actors.begin(); it != g_actors.end(); ++it)
        {
            if (*it == actor)
            {
                LeaveCriticalSection(&g_stateLock);
                return;
            }
        }
        if (g_actors.size() >= g_expectedActorCount)
        {
            LeaveCriticalSection(&g_stateLock);
            return;
        }
        actorIndex = g_actors.size();
        g_actors.push_back(actor);
        g_lastActorMs = GetTickCount();
        owner = g_captureOwner;
        hit = g_captureHit;
        actorCount = g_actors.size();
        captureComplete = actorCount >= g_expectedActorCount;
        if (captureComplete)
        {
            g_captureOwner = NULL;
            g_captureStartMs = 0;
            g_lastActorMs = 0;
            g_captureThreadId = 0;
            g_captureHit.valid = false;
            g_actors.clear();
        }
        LeaveCriticalSection(&g_stateLock);

        const ReactionArea reactionArea = ClassifyReactionArea(hit);
        const float variation = DeterministicSignedVariation(hit);
        const float horizontal = ClampFloat(
            (g_baseImpulse + hit.damage * g_damageScale) *
            HorizontalMultiplier(reactionArea) *
            (1.0f + variation * g_reactionVariation), 0.0f, g_maxImpulse);
        const float directionalVertical = ClampFloat(
            horizontal * g_swingDirectionCoefficient *
            SwingVerticalFactor(hit.cutDirection),
            -g_maxDirectionalVerticalImpulse, g_maxDirectionalVerticalImpulse);
        const float vertical = (g_upwardImpulse + directionalVertical) *
            LiftMultiplier(reactionArea);
        const float lateralAmount = SwingLateralFactor(hit.cutDirection) *
            g_swingDirectionCoefficient + variation * g_reactionVariation * 0.35f;
        float steeredX = hit.direction.x + hit.direction.z * lateralAmount;
        float steeredZ = hit.direction.z - hit.direction.x * lateralAmount;
        const float steeredLength = sqrtf(steeredX * steeredX + steeredZ * steeredZ);
        if (steeredLength > 0.0001f)
        {
            steeredX /= steeredLength;
            steeredZ /= steeredLength;
        }

        float spinMultiplier = SpinMultiplier(reactionArea);
        if (hit.cutDirection == CUT_THRUST || hit.cutDirection == CUT_PIERCED)
            spinMultiplier *= 0.25f;
        const float flairMagnitude = ClampFloat(horizontal *
            g_angularImpulseCoefficient * spinMultiplier, 0.0f, g_maxAngularImpulse);
        float flairX = -hit.direction.z;
        float flairY = 0.0f;
        float flairZ = hit.direction.x;
        const float lateralFactor = SwingLateralFactor(hit.cutDirection);
        if (lateralFactor != 0.0f)
        {
            flairX *= lateralFactor;
            flairZ *= lateralFactor;
            flairY = 0.20f;
        }
        const float verticalFactor = SwingVerticalFactor(hit.cutDirection);
        if (verticalFactor != 0.0f)
        {
            flairX = hit.direction.x * 0.65f;
            flairY = verticalFactor;
            flairZ = hit.direction.z * 0.65f;
        }
        const float flairLength = sqrtf(flairX * flairX + flairY * flairY + flairZ * flairZ);
        if (flairLength > 0.0001f)
        {
            flairX /= flairLength;
            flairY /= flairLength;
            flairZ /= flairLength;
        }
        const size_t variedIndex = (actorIndex + (variation > 0.0f ? 1 : 0)) %
            g_expectedActorCount;
        float flairFactor = (variedIndex & 1) ? 1.0f : -1.0f;
        if ((g_expectedActorCount & 1) && variedIndex == g_expectedActorCount - 1)
            flairFactor = 0.0f;
        const float share = 1.0f / static_cast<float>(g_expectedActorCount);
        const float flairShare = flairMagnitude * share * flairFactor;
        const ImpulseVector actorImpulse(
            steeredX * horizontal * share + flairX * flairShare,
            vertical * share + flairY * flairShare,
            steeredZ * horizontal * share + flairZ * flairShare);
        const bool applied = ApplyImpulseGuarded(actor, actorImpulse);

        if (g_debugLogging || !applied)
        {
            LogMessage(!applied,
                "actor_impulse_immediate source=%s character=%p actor=%p scene=%p index=%Iu/%Iu applied=%d complete=%d reaction=%s damage=%.2f impulse=(%.2f,%.2f,%.2f) thread=%lu",
                source, owner, actor, scene, actorIndex + 1, g_expectedActorCount,
                applied ? 1 : 0, captureComplete ? 1 : 0,
                ReactionAreaName(reactionArea), hit.damage, actorImpulse.x,
                actorImpulse.y, actorImpulse.z,
                static_cast<unsigned long>(GetCurrentThreadId()));
        }
    }

    size_t GetActorCount()
    {
        EnterCriticalSection(&g_stateLock);
        const size_t count = g_actors.size();
        LeaveCriticalSection(&g_stateLock);
        return count;
    }

    bool IsExecutableAddress(const void* address)
    {
        MEMORY_BASIC_INFORMATION info;
        if (!address || !VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        const DWORD protection = info.Protect & 0xFF;
        return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
            protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    }

    bool ApplyImpulseGuarded(NxActor* actor, const ImpulseVector& impulse)
    {
        typedef void (*AddForceFn)(NxActor*, const ImpulseVector&, int, bool);
        __try
        {
            void** vtable = *reinterpret_cast<void***>(actor);
            AddForceFn addForce = reinterpret_cast<AddForceFn>(vtable[NXACTOR_VTBL_ADD_FORCE]);
            if (!IsExecutableAddress(reinterpret_cast<void*>(addForce)))
                return false;
            addForce(actor, impulse, NX_FORCE_MODE_IMPULSE, true);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void ExpireIncompleteCapture()
    {
        Character* owner = NULL;
        size_t capturedCount = 0;
        EnterCriticalSection(&g_stateLock);
        if (g_captureOwner &&
            static_cast<DWORD>(GetTickCount() - g_captureStartMs) > g_captureGraceMs)
        {
            owner = g_captureOwner;
            capturedCount = g_actors.size();
            g_captureOwner = NULL;
            g_captureStartMs = 0;
            g_lastActorMs = 0;
            g_captureThreadId = 0;
            g_captureHit.valid = false;
            g_actors.clear();
        }
        LeaveCriticalSection(&g_stateLock);
        if (owner)
            LogMessage(true,
                "capture expired character=%p actors=%Iu/%Iu no_delayed_actor_calls=1",
                owner, capturedCount, g_expectedActorCount);
    }
}

HitMaterialType (*Character_hitByMeleeAttack_original)(
    Character*, CutDirection, Damages&, Character*, CombatTechniqueData*, int) = NULL;
GameData* (*MedicalSystem_addWound_original)(MedicalSystem*, bool, CutDirection,
    Damages&, int&, RootObject*, AttackDirection::Enum&, Harpoon*) = NULL;
bool (*Character__ragdollMode_original)(Character*, const Character::RagdollMsg&) = NULL;
NxJoint* (*Scythe_createJoint_original)(Scythe::PhysicsClass*, const Scythe::PhysicsJointData*,
    NxActor* const, NxActor* const, NxScene* const, const Scythe::Vector3&,
    const Scythe::Matrix&, const Scythe::Vector3&, int) = NULL;
NxJoint* (*Scythe_createJoint1_original)(Scythe::PhysicsClass*, const Scythe::PhysicsJointData*,
    NxActor* const, NxActor* const, NxScene* const, const Scythe::Vector3&,
    const Scythe::Matrix&, const Scythe::Vector3&, int) = NULL;

HitMaterialType Character_hitByMeleeAttack_hook(
    Character* victim, CutDirection direction, Damages& damage, Character* attacker,
    CombatTechniqueData* attack, int comboId)
{
    const HitMaterialType result = Character_hitByMeleeAttack_original(
        victim, direction, damage, attacker, attack, comboId);

    if (g_ready && victim && attacker)
    {
        RecentWound wound = {};
        bool woundMatched = false;
        EnterCriticalSection(&g_stateLock);
        if (g_wound.valid && g_wound.victim == victim &&
            g_wound.attacker == static_cast<RootObject*>(attacker) &&
            static_cast<DWORD>(GetTickCount() - g_wound.timeMs) <= 250)
        {
            wound = g_wound;
            woundMatched = true;
        }
        LeaveCriticalSection(&g_stateLock);

        RecentHit hit = {};
        hit.victim = victim;
        hit.attacker = attacker;
        hit.direction = HorizontalDirection(victim, attacker);
        hit.cutDirection = direction;
        hit.bodyPart = woundMatched ? wound.bodyPart : NULL;
        if (woundMatched)
            strncpy_s(hit.boneName, sizeof(hit.boneName), wound.boneName, _TRUNCATE);
        hit.damage = damage.total();
        hit.timeMs = GetTickCount();
        hit.valid = true;
        EnterCriticalSection(&g_stateLock);
        g_hit = hit;
        LeaveCriticalSection(&g_stateLock);
        if (g_debugLogging)
        {
            LogMessage(false,
                "melee_final victim=%p attacker=%p damage=%.2f cut=%s bodypart=%p bodypart_name=\"%s\" bone_name=\"%s\" direction=(%.3f,%.3f,%.3f)",
                victim, attacker, hit.damage, CutDirectionName(hit.cutDirection), hit.bodyPart,
                hit.bodyPart ? hit.bodyPart->name.c_str() : "<unknown>",
                hit.boneName[0] ? hit.boneName : "<unknown>", hit.direction.x,
                hit.direction.y, hit.direction.z);
        }
    }
    return result;
}

GameData* MedicalSystem_addWound_hook(MedicalSystem* self, bool lowBlow,
    CutDirection area, Damages& damage, int& material, RootObject* attacker,
    AttackDirection::Enum& attackDirection, Harpoon* harpoon)
{
    GameData* bodyPart = MedicalSystem_addWound_original(self, lowBlow, area,
        damage, material, attacker, attackDirection, harpoon);
    if (g_ready && self && self->me && attacker && bodyPart)
    {
        RecentWound wound = {};
        wound.victim = self->me;
        wound.attacker = attacker;
        wound.bodyPart = bodyPart;
        const std::string& boneName = MedicalSystem::getBoneNameForBodypart(bodyPart);
        strncpy_s(wound.boneName, sizeof(wound.boneName), boneName.c_str(), _TRUNCATE);
        wound.timeMs = GetTickCount();
        wound.valid = true;
        EnterCriticalSection(&g_stateLock);
        g_wound = wound;
        LeaveCriticalSection(&g_stateLock);
        if (g_debugLogging)
        {
            LogMessage(false,
                "wound_capture victim=%p attacker=%p bodypart=%p bodypart_name=\"%s\" bone_name=\"%s\" area=%s",
                self->me, attacker, bodyPart, bodyPart->name.c_str(), wound.boneName,
                CutDirectionName(area));
        }
    }
    return bodyPart;
}

NxJoint* Scythe_createJoint_hook(
    Scythe::PhysicsClass* self, const Scythe::PhysicsJointData* joint,
    NxActor* const actor0, NxActor* const actor1, NxScene* const scene,
    const Scythe::Vector3& position, const Scythe::Matrix& rotation,
    const Scythe::Vector3& scale, int jointParam)
{
    NxJoint* result = Scythe_createJoint_original(self, joint, actor0, actor1, scene,
        position, rotation, scale, jointParam);
    CaptureActor(actor0, "createJoint.actor0", scene);
    CaptureActor(actor1, "createJoint.actor1", scene);
    return result;
}

NxJoint* Scythe_createJoint1_hook(
    Scythe::PhysicsClass* self, const Scythe::PhysicsJointData* joint,
    NxActor* const actor0, NxActor* const actor1, NxScene* const scene,
    const Scythe::Vector3& position, const Scythe::Matrix& rotation,
    const Scythe::Vector3& scale, int jointParam)
{
    NxJoint* result = Scythe_createJoint1_original(self, joint, actor0, actor1, scene,
        position, rotation, scale, jointParam);
    CaptureActor(actor0, "createJoint1.actor0", scene);
    CaptureActor(actor1, "createJoint1.actor1", scene);
    return result;
}

bool Character__ragdollMode_hook(Character* character, const Character::RagdollMsg& message)
{
    const bool captureStarted = g_ready && message.on && BeginCapture(character);

    const bool result = Character__ragdollMode_original(character, message);

    if (captureStarted && g_debugLogging)
    {
        RecentHit hit;
        const bool matchingHit = TryGetRecentHit(character, &hit);
        LogMessage(false,
            "ragdoll_transition phase=end character=%p result=%d recent_melee=%d actor_count=%Iu"
            " damage=%.2f direction=(%.3f,%.3f,%.3f) awaiting_async_actors=1",
            character, result ? 1 : 0, matchingHit ? 1 : 0, GetActorCount(),
            matchingHit ? hit.damage : 0.0f,
            matchingHit ? hit.direction.x : 0.0f,
            matchingHit ? hit.direction.y : 0.0f,
            matchingHit ? hit.direction.z : 0.0f);
    }
    return result;
}

void (*GameWorld_mainLoop_original)(GameWorld*, float) = NULL;

void GameWorld_mainLoop_hook(GameWorld* world, float time)
{
    GameWorld_mainLoop_original(world, time);
    if (g_ready)
        ExpireIncompleteCapture();
}

__declspec(dllexport) void startPlugin()
{
    LoadConfig();
    InitializeCriticalSection(&g_stateLock);
    if (!g_enabled)
    {
        LogMessage(false, "disabled by configuration; no hooks installed");
        return;
    }

    void* hit = FindRealExport(
        "?_NV_hitByMeleeAttack@Character@@QEAA?AW4HitMaterialType@@W4CutDirection@@AEAVDamages@@PEAV1@PEAVCombatTechniqueData@@H@Z",
        "Character::_NV_hitByMeleeAttack");
    void* ragdoll = FindRealExport(
        "?_ragdollMode@Character@@QEAA_NAEBURagdollMsg@1@@Z",
        "Character::_ragdollMode");
    void* wound = FindRealExport(
        "?addWound@MedicalSystem@@QEAAPEAVGameData@@_NW4CutDirection@@AEAVDamages@@AEAHPEAVRootObject@@AEAW4Enum@AttackDirection@@PEAVHarpoon@@@Z",
        "MedicalSystem::addWound");
    void* joint = FindRealExport(
        "?createJoint@PhysicsClass@Scythe@@QEAAPEAVNxJoint@@PEBVPhysicsJointData@2@QEAVNxActor@@1QEAVNxScene@@AEBVVector3@2@AEBVMatrix@2@3H@Z",
        "Scythe::PhysicsClass::createJoint");
    void* joint1 = FindRealExport(
        "?createJoint1@PhysicsClass@Scythe@@QEAAPEAVNxJoint@@PEBVPhysicsJointData@2@QEAVNxActor@@1QEAVNxScene@@AEBVVector3@2@AEBVMatrix@2@3H@Z",
        "Scythe::PhysicsClass::createJoint1");
    void* mainLoop = FindRealExport(
        "?_NV_mainLoop_GPUSensitiveStuff@GameWorld@@QEAAXM@Z",
        "GameWorld::_NV_mainLoop_GPUSensitiveStuff");

    if (!hit || !ragdoll || !wound || !joint || !joint1 || !mainLoop)
    {
        g_enabled = false;
        LogMessage(true, "disabled before hook installation because a required export is missing");
        return;
    }

    const bool jointOk = InstallHook(joint, &Scythe_createJoint_hook,
        reinterpret_cast<void**>(&Scythe_createJoint_original), "Scythe::PhysicsClass::createJoint");
    const bool joint1Ok = InstallHook(joint1, &Scythe_createJoint1_hook,
        reinterpret_cast<void**>(&Scythe_createJoint1_original), "Scythe::PhysicsClass::createJoint1");
    const bool hitOk = InstallHook(hit, &Character_hitByMeleeAttack_hook,
        reinterpret_cast<void**>(&Character_hitByMeleeAttack_original), "Character::_NV_hitByMeleeAttack");
    const bool woundOk = InstallHook(wound, &MedicalSystem_addWound_hook,
        reinterpret_cast<void**>(&MedicalSystem_addWound_original), "MedicalSystem::addWound");
    const bool ragdollOk = InstallHook(ragdoll, &Character__ragdollMode_hook,
        reinterpret_cast<void**>(&Character__ragdollMode_original), "Character::_ragdollMode");
    const bool mainLoopOk = InstallHook(mainLoop, &GameWorld_mainLoop_hook,
        reinterpret_cast<void**>(&GameWorld_mainLoop_original),
        "GameWorld::_NV_mainLoop_GPUSensitiveStuff");

    g_ready = jointOk && joint1Ok && hitOk && woundOk && ragdollOk && mainLoopOk;
    if (!g_ready)
    {
        g_enabled = false;
        LogMessage(true, "inactive because one or more hooks failed; installed hooks remain pass-through");
        return;
    }

    LogMessage(false,
        "ready; mode=melee_ragdoll_actor_impulse physics=on teleport=never actor_calls=immediate_after_joint base=%.2f scale=%.2f max=%.2f up=%.2f swing_coeff=%.3f vertical_cap=%.2f distribution=fixed_share expected_actors=%Iu grace_ms=%lu",
        g_baseImpulse, g_damageScale, g_maxImpulse, g_upwardImpulse,
        g_swingDirectionCoefficient, g_maxDirectionalVerticalImpulse,
        g_expectedActorCount,
        static_cast<unsigned long>(g_captureGraceMs));
}
