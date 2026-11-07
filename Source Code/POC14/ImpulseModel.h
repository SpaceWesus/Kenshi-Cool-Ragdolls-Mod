#pragma once
#include <math.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Pure math/state only. No game pointers are dereferenced or retained here.
namespace ImpulseModel
{
    const size_t MaxActors = 64;
    inline bool Finite(float f) { return f == f && f <= FLT_MAX && f >= -FLT_MAX; }
    inline float Clamp(float f, float lo, float hi) { return f < lo ? lo : (f > hi ? hi : f); }
    struct Vec
    {
        float x, y, z;
        Vec(float a=0, float b=0, float c=0) : x(a), y(b), z(c) {}
        Vec operator*(float n) const { return Vec(x*n, y*n, z*n); }
        Vec operator+(const Vec& v) const { return Vec(x+v.x, y+v.y, z+v.z); }
        Vec cross(const Vec& v) const { return Vec(y*v.z-z*v.y,z*v.x-x*v.z,x*v.y-y*v.x); }
        float length() const { return sqrtf(x*x+y*y+z*z); }
    };
    enum Area { Unknown, Head, Torso, Arm, Leg };
    enum Cut { Straight, Left, Right, Down, Up };
    inline char Lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a'-'A') : c; }
    inline bool Equal(const char* a, const char* b)
    {
        if (!a || !b) return false;
        while (*a && *b && Lower(*a) == Lower(*b)) { ++a; ++b; }
        return !*a && !*b;
    }
    inline bool IsHuMRagdoll(const char* path)
    {
        if (!path) return false;
        const char* leaf=path;
        for (const char* p=path; *p; ++p) if (*p=='/' || *p=='\\') leaf=p+1;
        return Equal(leaf,"hu_m_ragdoll.phs");
    }
    inline bool Contains(const char* a, const char* b)
    {
        if (!a || !b || !*b) return false;
        for (; *a; ++a)
        {
            const char* p=a; const char* q=b;
            while (*p && *q && Lower(*p)==Lower(*q)) { ++p; ++q; }
            if (!*q) return true;
        }
        return false;
    }
    inline Area Classify(const char* name)
    {
        if (Contains(name,"head") || Contains(name,"neck")) return Head;
        if (Contains(name,"arm") || Contains(name,"hand") || Contains(name,"clavicle")) return Arm;
        if (Contains(name,"leg") || Contains(name,"thigh") || Contains(name,"calf") ||
            Contains(name,"foot") || Contains(name,"toe")) return Leg;
        if (Contains(name,"spine") || Contains(name,"chest") || Contains(name,"stomach") ||
            Contains(name,"pelvis") || Contains(name,"torso") || Contains(name,"belly")) return Torso;
        return Unknown;
    }
    inline int Side(const char* name)
    {
        if (Contains(name," L ") || Contains(name,"left")) return -1;
        if (Contains(name," R ") || Contains(name,"right")) return 1;
        return 0;
    }
    struct Config
    {
        float base, damageScale, maxImpulse, upward, swing, maxVertical, spread, maxSpeedChange;
        float rotationCoefficient, maxRotation, huMRagdollMultiplier;
        Config() : base(10), damageScale(15), maxImpulse(4000), upward(6),
            swing(.30f), maxVertical(1050), spread(.15f), maxSpeedChange(0),
            rotationCoefficient(1), maxRotation(1000), huMRagdollMultiplier(9.75f) {}
    };
    struct Hit
    {
        Vec direction;
        float damage;
        Cut cut;
        Area area;
        int side;
        char bone[96];
        Hit() : damage(0), cut(Straight), area(Unknown), side(0) { bone[0]=0; }
    };
    struct Body
    {
        float mass;
        char bone[96];
        Body() : mass(0) { bone[0]=0; }
    };
    struct Result
    {
        Vec impulse[MaxActors];
        Vec rotationalImpulse[MaxActors];
        float requestedHorizontal, deliveredHorizontal, scale;
        float requestedRotation, plannedRotation, assetMultiplier;
        size_t targets;
        enum Match { ExactBone, BodyArea, WholeBody } match;
        Result() : requestedHorizontal(0), deliveredHorizontal(0), scale(1),
            requestedRotation(0), plannedRotation(0), assetMultiplier(1), targets(0), match(WholeBody) {}
    };
    inline bool Build(const Config& c, const Hit& hit, const Body* bodies, size_t count, Result& out,
        const char* ragdollAsset=NULL)
    {
        out=Result();
        const float configValues[]={c.base,c.damageScale,c.maxImpulse,c.upward,c.swing,c.maxVertical,c.spread,c.maxSpeedChange,
            c.rotationCoefficient,c.maxRotation,c.huMRagdollMultiplier};
        for (size_t i=0;i<sizeof(configValues)/sizeof(configValues[0]);++i)
            if (!Finite(configValues[i]) || configValues[i]<0) return false;
        if (!count || count>MaxActors || !bodies || c.spread>1 ||
            c.huMRagdollMultiplier<1 || c.huMRagdollMultiplier>100 ||
            !Finite(hit.damage) || hit.damage<0 || !Finite(hit.direction.x) || !Finite(hit.direction.z)) return false;
        const float requested=c.base+hit.damage*c.damageScale;
        if (!Finite(requested)) return false;
        const float h=Clamp(requested,0,c.maxImpulse);
        const float len=sqrtf(hit.direction.x*hit.direction.x+hit.direction.z*hit.direction.z);
        if (!Finite(len) || len<.0001f) return false;
        const Vec forward(hit.direction.x/len,0,hit.direction.z/len);
        const float lateral=hit.cut==Left ? -c.swing : (hit.cut==Right ? c.swing : 0);
        Vec directed(forward.x+forward.z*lateral,0,forward.z-forward.x*lateral);
        directed=directed*(1.0f/directed.length());
        const float rise=hit.cut==Up ? c.swing : (hit.cut==Down ? -c.swing : 0);
        const float vertical=Clamp(c.upward+h*rise,-c.maxVertical,c.maxVertical);
        const Vec total(directed.x*h,vertical,directed.z*h);
        if (!Finite(total.length())) return false;
        bool target[MaxActors]={false};
        float mass=0, targetMass=0;
        for (size_t i=0;i<count;++i)
        {
            if (!Finite(bodies[i].mass) || bodies[i].mass<=0 || bodies[i].mass>1000000) return false;
            mass+=bodies[i].mass;
            if (hit.bone[0] && Equal(hit.bone,bodies[i].bone))
            { target[i]=true; targetMass+=bodies[i].mass; ++out.targets; }
        }
        out.match=Result::ExactBone;
        if (!out.targets)
        {
            out.match=Result::BodyArea;
            for (size_t i=0;i<count;++i)
                if (hit.area!=Unknown && Classify(bodies[i].bone)==hit.area &&
                    (!hit.side || Side(bodies[i].bone)==hit.side))
                { target[i]=true; targetMass+=bodies[i].mass; ++out.targets; }
        }
        if (!out.targets) out.match=Result::WholeBody;
        if (!Finite(mass) || mass<=0) return false;
        const float spread=out.targets ? c.spread : 1;
        for (size_t i=0;i<count;++i)
        {
            const float weight=spread*bodies[i].mass/mass +
                (target[i] ? (1-spread)*bodies[i].mass/targetMass : 0);
            out.impulse[i]=total*weight;
            const float length=out.impulse[i].length();
            // Zero disables this optional per-actor limiter; total/vertical caps remain.
            if (c.maxSpeedChange>0 && length>0)
                out.scale=Clamp(c.maxSpeedChange*bodies[i].mass/length,0,out.scale);
        }
        for (size_t i=0;i<count;++i) out.impulse[i]=out.impulse[i]*out.scale;
        out.requestedHorizontal=h;
        out.deliveredHorizontal=h*out.scale;
        // Approximate contact on the attacker-facing side of the struck actor.
        // Angular impulse = effective lever arm cross cut-directed impulse.
        // Do not treat constant UpwardImpulse as swing motion or invent twist for
        // a straight thrust / unknown hit region. No poses or area spin profiles.
        if (out.targets && hit.cut!=Straight && c.rotationCoefficient>0 && c.maxRotation>0)
        {
            const Vec lever=forward*(-c.rotationCoefficient);
            const Vec cutImpulse=Vec(total.x,vertical-Clamp(c.upward,-c.maxVertical,c.maxVertical),total.z)*out.scale;
            const Vec rotation=lever.cross(cutImpulse);
            out.requestedRotation=rotation.length();
            if (!Finite(out.requestedRotation)) return false;
            out.plannedRotation=Clamp(out.requestedRotation,0,c.maxRotation);
            if (out.requestedRotation>.000001f)
            {
                const Vec capped=rotation*(out.plannedRotation/out.requestedRotation);
                for (size_t i=0;i<count;++i)
                    if (target[i]) out.rotationalImpulse[i]=capped*(bodies[i].mass/targetMass);
            }
        }
        // Asset compensation is AFTER normal horizontal/vertical caps and angular
        // planning. Scale the whole linear vector without changing direction,
        // localization or the separately computed torque. Never reapply force caps.
        out.assetMultiplier=IsHuMRagdoll(ragdollAsset)?c.huMRagdollMultiplier:1;
        if (out.assetMultiplier!=1)
        {
            float extraScale=1;
            for (size_t i=0;i<count;++i)
            {
                out.impulse[i]=out.impulse[i]*out.assetMultiplier;
                const float length=out.impulse[i].length();
                if (!Finite(length)) return false;
                // A deliberately enabled velocity safety limit still bounds the
                // FINAL linear impulse. Default 0 leaves the full boost intact.
                if (c.maxSpeedChange>0 && length>0)
                    extraScale=Clamp(c.maxSpeedChange*bodies[i].mass/length,0,extraScale);
            }
            if (extraScale!=1)
                for (size_t i=0;i<count;++i) out.impulse[i]=out.impulse[i]*extraScale;
            out.scale*=extraScale;
            out.deliveredHorizontal=h*out.assetMultiplier*out.scale;
            if (!Finite(out.deliveredHorizontal)) return false;
        }
        return true;
    }

    // Owner/animation addresses are identity tokens only. The caller holds a lock.
    template<typename T, size_t Capacity> class HitStore
    {
    public:
        struct Entry
        {
            uintptr_t owner, animation;
            uint32_t at, armedAt;
            uint64_t serial;
            bool valid, pending, consumed;
            T hit;
            Entry() : owner(0),animation(0),at(0),armedAt(0),serial(0),valid(false),pending(false),consumed(false) {}
        };
        Entry entries[Capacity];
        uint64_t nextSerial;
        HitStore() : nextSerial(0) {}
        void Clear() { for (size_t i=0;i<Capacity;++i) entries[i]=Entry(); }
        Entry* Find(uintptr_t owner, uintptr_t animation)
        {
            for (size_t i=0;i<Capacity;++i)
                if (entries[i].valid && entries[i].owner==owner && entries[i].animation==animation) return &entries[i];
            return 0;
        }
        bool Record(uintptr_t owner, uintptr_t animation, const T& hit, uint32_t now, uint32_t ttl)
        {
            Entry* e=Find(owner,animation);
            if (e && e->pending && uint32_t(now-e->at)<=ttl) return false;
            if (!e) for (size_t i=0;i<Capacity;++i)
                if (!entries[i].valid || uint32_t(now-entries[i].at)>ttl) { e=&entries[i]; break; }
            if (!e) return false;
            *e=Entry(); e->owner=owner; e->animation=animation; e->hit=hit;
            e->at=now; e->serial=++nextSerial; e->valid=true; return true;
        }
        uint64_t Arm(uintptr_t owner, uintptr_t animation, uint32_t now, uint32_t ttl)
        {
            Entry* e=Find(owner,animation);
            if (!e || e->pending || e->consumed || uint32_t(now-e->at)>ttl) return 0;
            e->pending=true; e->armedAt=now; return e->serial;
        }
        void Cancel(uintptr_t owner, uintptr_t animation, uint64_t serial)
        {
            Entry* e=Find(owner,animation);
            if (e && (!serial || e->serial==serial)) { e->pending=false; e->consumed=true; }
        }
        bool Take(uintptr_t owner, uintptr_t animation, uint32_t now, uint32_t ttl, T& hit, uint64_t& serial)
        {
            Entry* e=Find(owner,animation);
            if (!e || !e->pending || e->consumed || uint32_t(now-e->at)>ttl) return false;
            e->pending=false; e->consumed=true; hit=e->hit; serial=e->serial; return true;
        }
    };
}
