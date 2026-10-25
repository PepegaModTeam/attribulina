// A stand-in for the game's Attrib and UMath types, sized as the PC builds
// have them, for compiling the Attrib::Gen headers Attribulina writes
// (check_gen_headers.sh). Declarations only, from the 360 debug builds' PDBs
// and Most Wanted's DWARF dump; define ATTRIB_GEN_MW for Most Wanted's.

#pragma once

#include <string.h>

namespace UTL
{
namespace COM
{
struct IUnknown;
}
} // namespace UTL

namespace UMath
{
struct Vector2
{
    float x, y;
};
struct Vector3
{
    float x, y, z;
};
struct alignas(16) Vector4
{
    float x, y, z, w;
};
struct alignas(16) Matrix4
{
    Vector4 v0, v1, v2, v3;
};
} // namespace UMath

namespace Attrib
{
class Collection;

#ifdef ATTRIB_GEN_MW
struct StringKey
{
    unsigned long long mHash64;
    unsigned int mHash32;
    const char* mString;
};
#else
struct StringKey
{
    unsigned int mHash32;
    const char* mString;
};
#endif

struct RefSpec
{
    unsigned int mClassKey;
    unsigned int mCollectionKey;
    const Collection* mCollectionPtr;
};

struct Blob
{
    unsigned int mSize;
    const void* mData;
};

// An Attrib::Array header: u16 mAlloc, mCount, mSize, mEncodedTypeAndPad.
class Private
{
public:
    unsigned int GetLength() const
    {
        unsigned short n;
        memcpy(&n, mData + 2, 2);
        return n;
    }

private:
    unsigned char mData[8];
};

class Attribute
{
public:
    unsigned int GetLength() const;
};

template <class T> class TAttrib
{
public:
    TAttrib();
    explicit TAttrib(const Attribute& a);
    bool IsValid() const;
    bool Set(unsigned int index, const T& value);
};

const Collection* FindCollection(unsigned int classKey, unsigned int collectionKey);
const Collection* FindCollectionWithDefault(unsigned int classKey, unsigned int collectionKey);
const void* DefaultDataArea(unsigned int bytes);

class Instance
{
public:
#ifdef ATTRIB_GEN_MW
    Instance(const Collection* collection, unsigned int msgPort, UTL::COM::IUnknown* owner);
    Instance(const RefSpec& refspec, unsigned int msgPort, UTL::COM::IUnknown* owner);
#else
    Instance(const Collection* collection, unsigned int msgPort);
    Instance(const RefSpec& refspec, unsigned int msgPort);
#endif
    Instance(const Instance& src);
    ~Instance();
    Instance& operator=(const Instance& rhs);
    void Change(const Collection* c);
    void Change(const RefSpec& refspec);
    void ChangeWithDefault(const RefSpec& refspec);
    const Collection* GetConstCollection() const;
    bool IsValid() const;
    bool IsDynamic() const;
    Attribute Get(unsigned int attributeKey) const;

protected:
    const void* GetAttributePointer(unsigned int attributeKey, unsigned int index) const;
    void* GetLayoutPointer() const;
    void SetDefaultLayout(unsigned int bytes);
    bool ModifyInternal(unsigned int classKey, unsigned int dynamicCollectionKey, unsigned int spaceForAdditionalAttributes);
#ifdef ATTRIB_GEN_MW
    unsigned int GUKeyInternal(unsigned int classKey, const char* name, bool registerName) const;
#else
    unsigned int GUKeyInternal(unsigned int classKey, const char* name) const;
#endif
    template <class T> TAttrib<T> GetOrClone(unsigned int attributeKey);

private:
#ifdef ATTRIB_GEN_MW
    UTL::COM::IUnknown* mOwner;
#endif
    const Collection* mCollection;
    void* mLayoutPtr;
    unsigned int mMsgPort;
    unsigned int mFlags;
};
} // namespace Attrib
