#pragma once

#ifdef _KERNEL_MODE
#include <ntimage.h>
#else
#include <winnt.h>
#endif

namespace Pe
{



enum class Arch : unsigned char
{
    unknown,
    x32,
    x64,
    native = (sizeof(size_t) == sizeof(unsigned long) ? x32 : x64),
    inverse = (native == x32 ? x64 : x32)
};

enum class ImgType : unsigned char
{
    file,
    module
};

using Rva = unsigned int;
using Ordinal = unsigned short;

enum class ImportType
{
    unknown,
    name,
    ordinal
};

enum class ExportType
{
    unknown,
    exact,
    forwarder
};

enum class RelocType
{
    unknown,
    absolute,
    high,
    low,
    highlow,
    highadj,
    dir64
};

struct Reloc
{
    unsigned short offsetInPage : 12;
    unsigned short rawType : 4; // IMG_REL_BASED_***

    RelocType type() const noexcept
    {
        switch (rawType)
        {
        case IMAGE_REL_BASED_ABSOLUTE : return RelocType::absolute;
        case IMAGE_REL_BASED_HIGH     : return RelocType::high;
        case IMAGE_REL_BASED_LOW      : return RelocType::low;
        case IMAGE_REL_BASED_HIGHLOW  : return RelocType::highlow;
        case IMAGE_REL_BASED_HIGHADJ  : return RelocType::highadj;
        case IMAGE_REL_BASED_DIR64    : return RelocType::dir64;
        default:
            return RelocType::unknown;
        }
    }
};
static_assert(sizeof(Reloc) == sizeof(unsigned short));

struct GenericTypes
{
    using DosHeader = IMAGE_DOS_HEADER;
    using ImgDataDir = IMAGE_DATA_DIRECTORY;
    using SecHeader = IMAGE_SECTION_HEADER;
    using ImgImportByName = IMAGE_IMPORT_BY_NAME;
    using BoundForwarderRef = IMAGE_BOUND_FORWARDER_REF;
    
    struct RUNTIME_FUNCTION // For x86 headers compatibility
    {
        unsigned int BeginAddress;
        unsigned int EndAddress;
        union
        {
            unsigned int UnwindInfoAddress;
            unsigned int UnwindData;
        } UnwindInfo;
    };

    union Eat
    {
        Rva address;
        Rva forwarderString;
    };

    struct RelocsTable
    {
        struct Header
        {
            Rva pageRva;
            unsigned int relocsSizeInBytes;
        } hdr;

        Reloc relocs[1];

        unsigned int count() const noexcept
        {
            return hdr.relocsSizeInBytes / sizeof(Reloc);
        }
    };
};

template <Arch arch>
struct Types;

template <>
struct Types<Arch::x32> : public GenericTypes
{
    using NtHeaders = IMAGE_NT_HEADERS32;
    using OptHeader = IMAGE_OPTIONAL_HEADER32;
    using ImgThunkData = IMAGE_THUNK_DATA32;
    union Iat
    {
        unsigned int raw;
        ImgThunkData thunk;
        struct
        {
            Rva hintNameRva : 31;
        } name;
        struct
        {
            unsigned int ord : 16;
        } ordinal;
        unsigned int reserved : 31;
        unsigned int importByOrdinal : 1;

        bool valid() const noexcept
        {
            return raw != 0;
        }

        ImportType type() const noexcept
        {
            if (!valid())
            {
                return ImportType::unknown;
            }
            return importByOrdinal ? ImportType::ordinal : ImportType::name;
        }
    };
    static_assert(sizeof(Iat) == sizeof(unsigned int));
    using Ilt = Iat;
    using ImportNameTable = Iat;
    
    static constexpr auto k_magic = 0x010Bu; // PE32
};

template <>
struct Types<Arch::x64> : public GenericTypes
{
    using NtHeaders = IMAGE_NT_HEADERS64;
    using OptHeader = IMAGE_OPTIONAL_HEADER64;
    using ImgThunkData = IMAGE_THUNK_DATA64;
    union Iat
    {
        unsigned long long raw;
        ImgThunkData thunk;
        struct
        {
            Rva hintNameRva : 31;
        } name;
        struct
        {
            unsigned long long ord : 16;
        } ordinal;
        unsigned long long reserved : 63;
        unsigned long long importByOrdinal : 1;

        bool valid() const noexcept
        {
            return raw != 0;
        }

        ImportType type() const noexcept
        {
            if (!valid())
            {
                return ImportType::unknown;
            }
            return importByOrdinal ? ImportType::ordinal : ImportType::name;
        }
    };
    static_assert(sizeof(Iat) == sizeof(unsigned long long));
    using Ilt = Iat;
    using ImportNameTable = Iat;

    static constexpr auto k_magic = 0x020Bu; // PE32+
};

template <typename DirType, unsigned int id>
struct Dir
{
    using Type = DirType;
    static constexpr auto k_id = id;
};

using DirImports = Dir<IMAGE_IMPORT_DESCRIPTOR, IMAGE_DIRECTORY_ENTRY_IMPORT>;
using DirDelayedImports = Dir<IMAGE_DELAYLOAD_DESCRIPTOR, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT>;
using DirBoundImports = Dir<IMAGE_BOUND_IMPORT_DESCRIPTOR, IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT>;
using DirExports = Dir<IMAGE_EXPORT_DIRECTORY, IMAGE_DIRECTORY_ENTRY_EXPORT>;
using DirRelocs  = Dir<IMAGE_BASE_RELOCATION, IMAGE_DIRECTORY_ENTRY_BASERELOC>;
using DirExceptions = Dir<GenericTypes::RUNTIME_FUNCTION, IMAGE_DIRECTORY_ENTRY_EXCEPTION>;

class Sections;
class Imports;
class DelayedImports;
class BoundImports;
class Exports;
class Relocs;
class Exceptions;


struct PeMagic
{
    static constexpr auto k_mz = 0x5A4Dui16; // MZ
    static constexpr auto k_pe = 0x00004550ui32; // "PE\0\0"
};

template <Arch arch>
class PeHeaders : public PeMagic
{
public:
    using Types = Types<arch>;
    using DosHeader = typename Types::DosHeader;
    using NtHeaders = typename Types::NtHeaders;
    using OptHeader = typename Types::OptHeader;
    
public:
    static constexpr auto k_magic = Types::k_magic;

private:
    const void* const m_base;

public:
    explicit PeHeaders(const void* const base) noexcept : m_base(base)
    {
    }

    const DosHeader* dos() const noexcept
    {
        return static_cast<const DosHeader*>(m_base);
    }

    const NtHeaders* nt() const noexcept
    {
        return reinterpret_cast<const NtHeaders*>(static_cast<const unsigned char*>(m_base) + dos()->e_lfanew);
    }

    const OptHeader* opt() const noexcept
    {
        return &nt()->OptionalHeader;
    }

    const void* mod() const noexcept
    {
        return m_base;
    }

    bool valid() const noexcept
    {
        const auto* const dosHdr = dos();
        if (!dosHdr)
        {
            return false;
        }

        if (dosHdr->e_magic != k_mz)
        {
            return false;
        }

        const auto* const ntHdr = nt();
        if (ntHdr->Signature != k_pe)
        {
            return false;
        }

        const auto* const optHdr = opt();
        if (optHdr->Magic != k_magic)
        {
            return false;
        }

        return true;
    }
};

struct PeArch
{
    static Arch classify(const void* const base) noexcept
    {
        if (PeHeaders<Arch::native>(base).valid())
        {
            return Arch::native;
        }
        else if (PeHeaders<Arch::inverse>(base).valid())
        {
            return Arch::inverse;
        }
        else
        {
            return Arch::unknown;
        }
    }
};


struct Aligner
{
    template <typename Type>
    static constexpr Type alignDown(const Type value, const Type factor) noexcept
    {
        return value & ~(factor - 1);
    }

    template <typename Type>
    static constexpr Type alignUp(const Type value, const Type factor) noexcept
    {
        return alignDown<Type>(value - 1, factor) + factor;
    }
};


class Pe : protected Aligner
{
public:
    using ImgDataDir = typename GenericTypes::ImgDataDir;

private:
    const void* const m_base;
    const Arch m_arch;
    const ImgType m_type;

public:
    Pe(const ImgType type, const void* const base) noexcept : m_base(base), m_arch(PeArch::classify(base)), m_type(type)
    {
    }

    static Pe fromFile(const void* const base) noexcept
    {
        return Pe(ImgType::file, base);
    }

    static Pe fromModule(const void* const base) noexcept
    {
        return Pe(ImgType::module, base);
    }

    template <Arch arch>
    PeHeaders<arch> headers() const noexcept
    {
        if (arch == m_arch)
        {
            return PeHeaders<arch>(m_base);
        }
        else
        {
            return PeHeaders<arch>(nullptr);
        }
    }

    template <typename Type>
    const Type* byRva(const Rva rva) const noexcept
    {
        if (m_type == ImgType::module)
        {
            return reinterpret_cast<const Type*>(static_cast<const unsigned char*>(m_base) + rva);
        }

        unsigned int fileAlignment = 0;
        unsigned int sectionAlignment = 0;
        switch (arch())
        {
        case Arch::native:
        {
            const auto* optHdr = headers<Arch::native>().opt();
            fileAlignment = optHdr->FileAlignment;
            sectionAlignment = optHdr->SectionAlignment;
            break;
        }
        case Arch::inverse:
        {
            const auto* optHdr = headers<Arch::inverse>().opt();
            fileAlignment = optHdr->FileAlignment;
            sectionAlignment = optHdr->SectionAlignment;
            break;
        }
        default:
        {
            return nullptr;
        }
        }

        constexpr auto k_minimalSectionAlignment = 512u;
        for (const auto& sec : sections())
        {
            const auto sizeOnDisk = sec.SizeOfRawData;
            const auto sizeInMem = sec.Misc.VirtualSize;

            unsigned long long sectionBase = 0;
            unsigned long long sectionSize = 0;
            unsigned long long sectionOffset = 0;
            if (sectionAlignment >= k_minimalSectionAlignment)
            {
                sectionBase = alignDown<unsigned long long>(sec.VirtualAddress, sectionAlignment);
                const auto alignedFileSize = alignUp<unsigned long long>(sizeOnDisk, fileAlignment);
                const auto alignedSectionSize = alignUp<unsigned long long>(sizeInMem, sectionAlignment);
                sectionSize = (alignedFileSize > alignedSectionSize) ? alignedSectionSize : alignedFileSize;
                sectionOffset = alignDown<unsigned long long>(sec.PointerToRawData, k_minimalSectionAlignment);
            }
            else
            {
                sectionBase = sec.VirtualAddress;
                sectionSize = (sizeOnDisk > sizeInMem) ? sizeInMem : sizeOnDisk;
                sectionOffset = sec.PointerToRawData;
            }

            if ((rva >= sectionBase) && (rva < sectionBase + sectionSize))
            {
                return reinterpret_cast<const Type*>(static_cast<const unsigned char*>(m_base) + (sectionOffset + (rva - sectionBase)));
            }
        }
        
        return nullptr;
    }

    template <typename Type>
    const Type* byOffset(const unsigned int offset) const noexcept
    {
        return reinterpret_cast<const Type*>(reinterpret_cast<const unsigned char*>(m_base) + offset);
    }

    const ImgDataDir* dir(const unsigned int id) const noexcept
    {
        switch (m_arch)
        {
        case Arch::native:
        {
            return &headers<Arch::native>().opt()->DataDirectory[id];
        }
        case Arch::inverse:
        {
            return &headers<Arch::inverse>().opt()->DataDirectory[id];
        }
        default:
        {
            return nullptr;
        }
        }
    }

    template <typename DirType>
    typename const typename DirType::Type* dir() const noexcept
    {
        const auto* const dirHdr = dir(DirType::k_id);
        if (!dirHdr->Size)
        {
            return nullptr;
        }

        return byRva<typename DirType::Type>(dirHdr->VirtualAddress);
    }

    unsigned long long imageBase() const noexcept
    {
        switch (m_arch)
        {
        case Arch::native:
        {
            return headers<Arch::native>().opt()->ImageBase;
        }
        case Arch::inverse:
        {
            return headers<Arch::inverse>().opt()->ImageBase;
        }
        default:
        {
            return 0;
        }
        }
    }

    unsigned long imageSize() const noexcept
    {
        switch (m_arch)
        {
        case Arch::native:
        {
            return headers<Arch::native>().opt()->SizeOfImage;
        }
        case Arch::inverse:
        {
            return headers<Arch::inverse>().opt()->SizeOfImage;
        }
        default:
        {
            return 0;
        }
        }
    }

    unsigned long long entryPoint() const noexcept
    {
        switch (m_arch)
        {
        case Arch::native:
        {
            return static_cast<unsigned long long>(reinterpret_cast<size_t>(byRva<void>(headers<Arch::native>().opt()->AddressOfEntryPoint)));
        }
        case Arch::inverse:
        {
            return static_cast<unsigned long long>(reinterpret_cast<size_t>(byRva<void>(headers<Arch::inverse>().opt()->AddressOfEntryPoint)));
        }
        default:
        {
            return 0;
        }
        }
    }

    Arch arch() const noexcept
    {
        return m_arch;
    }

    ImgType type() const noexcept
    {
        return m_type;
    }

    bool valid() const noexcept
    {
        return arch() != Arch::unknown;
    }

    Sections sections() const noexcept;
    Imports imports() const noexcept;
    DelayedImports delayedImports() const noexcept;
    BoundImports boundImports() const noexcept;
    Exports exports() const noexcept;
    Relocs relocs() const noexcept;
    Exceptions exceptions() const noexcept;
};



class Sections
{
public:
    class Iterator
    {
    private:
        const Sections& m_owner;
        unsigned int m_pos;

    public:
        Iterator(const Sections& owner, const unsigned int pos) noexcept
            : m_owner(owner)
            , m_pos(pos)
        {
            if (m_pos > m_owner.count())
            {
                m_pos = m_owner.count();
            }
        }

        Iterator& operator ++ () noexcept
        {
            if (m_pos < m_owner.count())
            {
                ++m_pos;
            }

            return *this;
        }

        Iterator operator ++ (int) noexcept
        {
            const auto it = *this;
            ++(*this);
            return it;
        }

        bool operator == (const Iterator& it) const noexcept
        {
            return (m_pos == it.m_pos) && (m_owner.sections() == it.m_owner.sections());
        }

        bool operator != (const Iterator& it) const noexcept
        {
            return !operator == (it);
        }

        const typename GenericTypes::SecHeader& operator * () const noexcept
        {
            return *operator -> ();
        }

        const typename GenericTypes::SecHeader* operator -> () const noexcept
        {
            return &m_owner.sections()[m_pos];
        }
    };

private:
    const typename GenericTypes::SecHeader* const m_sections;
    const unsigned int m_count;

public:
    Sections(const typename GenericTypes::SecHeader* const sections, const unsigned int count) noexcept
        : m_sections(sections)
        , m_count(count)
    {
    }

    const typename GenericTypes::SecHeader* sections() const noexcept
    {
        return m_sections;
    }

    bool valid() const noexcept
    {
        return m_sections != nullptr;
    }

    bool empty() const noexcept
    {
        return !valid() || !m_count;
    }

    unsigned int count() const noexcept
    {
        return m_count;
    }

    Iterator begin() const noexcept
    {
        return Iterator(*this, 0);
    }

    Iterator end() const noexcept
    {
        return Iterator(*this, m_count);
    }
};



template <typename EntryType>
class GenericIterator
{
private:
    EntryType m_entry;

public:
    template <typename... Args>
    GenericIterator(const Args&... args) noexcept : m_entry(args...)
    {
    }

    GenericIterator& operator ++ () noexcept
    {
        m_entry.step();
        return *this;
    }

    GenericIterator operator ++ (int) noexcept
    {
        const auto it = *this;
        ++(*this);
        return it;
    }

    bool operator == (const GenericIterator& it) const noexcept
    {
        return m_entry.equals(it.m_entry);
    }

    bool operator != (const GenericIterator& it) const noexcept
    {
        return !operator == (it);
    }

    const EntryType& operator * () const noexcept
    {
        return m_entry;
    }

    const EntryType* operator -> () const noexcept
    {
        return &m_entry;
    }
};



class Imports
{
public:
    class LibEntry;

    class FuncEntry
    {
    public:
        static constexpr auto k_invalid = 0xFFFFFFFFu;

    private:
        const LibEntry& m_lib;
        unsigned int m_index;

    public:
        FuncEntry(const LibEntry& lib, const unsigned int index) noexcept : m_lib(lib), m_index(index)
        {
        }

        const LibEntry& lib() const noexcept
        {
            return m_lib;
        }

        unsigned int index() const noexcept
        {
            return m_index;
        }

        template <Arch arch>
        const typename Types<arch>::Iat* iat() const noexcept
        {
            if ((arch != m_lib.pe().arch()) || (m_index == k_invalid))
            {
                return nullptr;
            }

            return &m_lib.iat<arch>()[m_index];
        }

        template <Arch arch>
        const typename Types<arch>::Ilt* ilt() const noexcept
        {
            if ((arch != m_lib.pe().arch()) || (m_index == k_invalid))
            {
                return nullptr;
            }

            return &m_lib.ilt<arch>()[m_index];
        }

        bool valid() const noexcept
        {
            if (m_index == k_invalid)
            {
                return false;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return ilt<Arch::native>()->valid();
            }
            case Arch::inverse:
            {
                return ilt<Arch::inverse>()->valid();
            }
            default:
            {
                return false;
            }
            }
        }

        bool empty() const noexcept
        {
            return !valid();
        }

        ImportType type() const noexcept
        {
            if (m_index == k_invalid)
            {
                return ImportType::unknown;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return ilt<Arch::native>()->type();
            }
            case Arch::inverse:
            {
                return ilt<Arch::inverse>()->type();
            }
            default:
            {
                return ImportType::unknown;
            }
            }
        }

        const typename GenericTypes::ImgImportByName* name() const noexcept
        {
            if (type() != ImportType::name)
            {
                return nullptr;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                const Rva rva = ilt<Arch::native>()->name.hintNameRva;
                return m_lib.pe().byRva<typename GenericTypes::ImgImportByName>(rva);
            }
            case Arch::inverse:
            {
                const Rva rva = ilt<Arch::inverse>()->name.hintNameRva;
                return m_lib.pe().byRva<typename GenericTypes::ImgImportByName>(rva);
            }
            default:
            {
                return 0;
            }
            }
        }

        unsigned long long address() const noexcept
        {
            if ((m_lib.pe().type() == ImgType::file) && !m_lib.bound())
            {
                return 0;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return iat<Arch::native>()->raw;
            }
            case Arch::inverse:
            {
                return iat<Arch::inverse>()->raw;
            }
            default:
            {
                return 0;
            }
            }
        }

        unsigned short ordinal() const noexcept
        {
            if (type() != ImportType::ordinal)
            {
                return 0;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return ilt<Arch::native>()->ordinal.ord;
            }
            case Arch::inverse:
            {
                return ilt<Arch::inverse>()->ordinal.ord;
            }
            default:
            {
                return 0;
            }
            }
        }

        bool equals(const FuncEntry& entry) const noexcept
        {
            if (&m_lib != &entry.m_lib)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return index() == entry.index();
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_index;
            }
        }
    };

    using FuncIterator = GenericIterator<FuncEntry>;

    class LibEntry
    {
    private:
        const Pe& m_pe;
        const typename DirImports::Type* m_desc;

    public:
        LibEntry(const Pe& pe, const typename DirImports::Type* const desc) noexcept
            : m_pe(pe)
            , m_desc(desc)
        {
        }

        const Pe& pe() const noexcept
        {
            return m_pe;
        }

        const typename DirImports::Type* desc() const noexcept
        {
            return m_desc;
        }

        bool valid() const noexcept
        {
            return m_desc && m_desc->Characteristics;
        }

        bool empty() const noexcept
        {
            return !valid();
        }

        const char* libName() const noexcept
        {
            return m_pe.byRva<char>(m_desc->Name);
        }

        // Import Address Table:
        template <Arch arch>
        const typename Types<arch>::Iat* iat() const noexcept
        {
            if (arch == m_pe.arch())
            {
                return m_pe.byRva<typename Types<arch>::Iat>(m_desc->FirstThunk);
            }
            else
            {
                return nullptr;
            }
        }

        // Import Lookup Table:
        template <Arch arch>
        const typename Types<arch>::Ilt* ilt() const noexcept
        {
            if (arch == m_pe.arch())
            {
                return m_pe.byRva<typename Types<arch>::Ilt>(m_desc->OriginalFirstThunk);
            }
            else
            {
                return nullptr;
            }
        }

        bool bound() const noexcept
        {
            return desc()->TimeDateStamp != 0;
        }

        bool equals(const LibEntry& entry) const noexcept
        {
            if (&m_pe != &entry.m_pe)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return desc() == entry.desc();
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_desc;
            }
        }

        FuncIterator begin() const noexcept
        {
            if (!valid())
            {
                return end();
            }

            return FuncIterator(*this, 0);
        }

        FuncIterator end() const noexcept
        {
            return FuncIterator(*this, FuncEntry::k_invalid);
        }
    };

    using LibIterator = GenericIterator<LibEntry>;


private:
    const Pe& m_pe;

public:
    explicit Imports(const Pe& pe) noexcept : m_pe(pe)
    {
    }

    const Pe& pe() const noexcept
    {
        return m_pe;
    }

    const typename DirImports::Type* desc() const noexcept
    {
        return m_pe.dir<DirImports>();
    }

    bool valid() const noexcept
    {
        return desc() != nullptr;
    }

    bool empty() const noexcept
    {
        const auto* const impDesc = desc();
        return !impDesc || !impDesc->FirstThunk;
    }

    LibIterator begin() const noexcept
    {
        return LibIterator(m_pe, desc());
    }

    LibIterator end() const noexcept
    {
        return LibIterator(m_pe, nullptr);
    }
};



class DelayedImports
{
public:
    class LibEntry;

    class FuncEntry
    {
    public:
        static constexpr auto k_invalid = 0xFFFFFFFFu;

    private:
        const LibEntry& m_lib;
        unsigned int m_index;

    public:
        FuncEntry(const LibEntry& lib, const unsigned int index) noexcept : m_lib(lib), m_index(index)
        {
        }

        const LibEntry& lib() const noexcept
        {
            return m_lib;
        }

        unsigned int index() const noexcept
        {
            return m_index;
        }

        template <Arch arch>
        const typename Types<arch>::Iat* iat() const noexcept
        {
            if ((arch != m_lib.pe().arch()) || (m_index == k_invalid))
            {
                return nullptr;
            }

            return &m_lib.iat<arch>()[m_index];
        }

        template <Arch arch>
        const typename Types<arch>::ImportNameTable* nameEntry() const noexcept
        {
            if ((arch != m_lib.pe().arch()) || (m_index == k_invalid))
            {
                return nullptr;
            }

            return &m_lib.names<arch>()[m_index];
        }

        bool valid() const noexcept
        {
            if (m_index == k_invalid)
            {
                return false;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return nameEntry<Arch::native>()->valid();
            }
            case Arch::inverse:
            {
                return nameEntry<Arch::inverse>()->valid();
            }
            default:
            {
                return false;
            }
            }
        }

        bool empty() const noexcept
        {
            return !valid();
        }

        ImportType type() const noexcept
        {
            if (m_index == k_invalid)
            {
                return ImportType::unknown;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return nameEntry<Arch::native>()->type();
            }
            case Arch::inverse:
            {
                return nameEntry<Arch::inverse>()->type();
            }
            default:
            {
                return ImportType::unknown;
            }
            }
        }

        const typename GenericTypes::ImgImportByName* name() const noexcept
        {
            if (type() != ImportType::name)
            {
                return nullptr;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                const Rva rva = nameEntry<Arch::native>()->name.hintNameRva;
                return m_lib.pe().byRva<typename GenericTypes::ImgImportByName>(rva);
            }
            case Arch::inverse:
            {
                const Rva rva = nameEntry<Arch::inverse>()->name.hintNameRva;
                return m_lib.pe().byRva<typename GenericTypes::ImgImportByName>(rva);
            }
            default:
            {
                return 0;
            }
            }
        }

        unsigned long long address() const noexcept
        {
            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return iat<Arch::native>()->raw;
            }
            case Arch::inverse:
            {
                return iat<Arch::inverse>()->raw;
            }
            default:
            {
                return 0;
            }
            }
        }

        unsigned int ordinal() const noexcept
        {
            if (type() != ImportType::ordinal)
            {
                return 0;
            }

            switch (m_lib.pe().arch())
            {
            case Arch::native:
            {
                return nameEntry<Arch::native>()->ordinal.ord;
            }
            case Arch::inverse:
            {
                return nameEntry<Arch::inverse>()->ordinal.ord;
            }
            default:
            {
                return 0;
            }
            }
        }

        bool equals(const FuncEntry& entry) const noexcept
        {
            if (&m_lib != &entry.m_lib)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return index() == entry.index();
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_index;
            }
        }
    };

    using FuncIterator = GenericIterator<FuncEntry>;

    class LibEntry
    {
    private:
        const Pe& m_pe;
        const typename DirDelayedImports::Type* m_desc;

    public:
        LibEntry(const Pe& pe, const typename DirDelayedImports::Type* const desc) noexcept
            : m_pe(pe)
            , m_desc(desc)
        {
        }

        const Pe& pe() const noexcept
        {
            return m_pe;
        }

        const typename DirDelayedImports::Type* desc() const noexcept
        {
            return m_desc;
        }

        bool valid() const noexcept
        {
            return m_desc && m_desc->DllNameRVA;
        }

        bool empty() const noexcept
        {
            return !valid();
        }

        const char* libName() const noexcept
        {
            return m_pe.byRva<char>(m_desc->DllNameRVA);
        }

        // Import Address Table:
        template <Arch arch>
        const typename Types<arch>::Iat* iat() const noexcept
        {
            if (arch == m_pe.arch())
            {
                return m_pe.byRva<typename Types<arch>::Iat>(m_desc->ImportAddressTableRVA);
            }
            else
            {
                return nullptr;
            }
        }

        // Import Name Table:
        template <Arch arch>
        const typename Types<arch>::ImportNameTable* names() const noexcept
        {
            if (arch == m_pe.arch())
            {
                return m_pe.byRva<typename Types<arch>::ImportNameTable>(m_desc->ImportNameTableRVA);
            }
            else
            {
                return nullptr;
            }
        }

        bool equals(const LibEntry& entry) const noexcept
        {
            if (&m_pe != &entry.m_pe)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return desc() == entry.desc();
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_desc;
            }
        }

        FuncIterator begin() const noexcept
        {
            if (!valid())
            {
                return end();
            }

            return FuncIterator(*this, 0);
        }

        FuncIterator end() const noexcept
        {
            return FuncIterator(*this, FuncEntry::k_invalid);
        }
    };

    using LibIterator = GenericIterator<LibEntry>;

private:
    const Pe& m_pe;

public:
    explicit DelayedImports(const Pe& pe) noexcept : m_pe(pe)
    {
    }

    const Pe& pe() const noexcept
    {
        return m_pe;
    }

    const typename DirDelayedImports::Type* desc() const noexcept
    {
        return m_pe.dir<DirDelayedImports>();
    }

    bool valid() const noexcept
    {
        return desc() != nullptr;
    }

    bool empty() const noexcept
    {
        const auto* const impDesc = desc();
        return !impDesc || !impDesc->DllNameRVA;
    }

    LibIterator begin() const noexcept
    {
        return LibIterator(m_pe, desc());
    }

    LibIterator end() const noexcept
    {
        return LibIterator(m_pe, nullptr);
    }
};



class BoundImports
{
public:
    class LibEntry;

    class ForwarderEntry
    {
    public:
        static constexpr auto k_invalid = 0xFFFFFFFFu;

    private:
        const LibEntry& m_lib;
        unsigned int m_index;

    public:
        ForwarderEntry(const LibEntry& lib, const unsigned int index) noexcept : m_lib(lib), m_index(index)
        {
        }

        const LibEntry& lib() const noexcept
        {
            return m_lib;
        }

        unsigned int index() const noexcept
        {
            return m_index;
        }

        const typename GenericTypes::BoundForwarderRef* desc() const noexcept
        {
            if (m_index == k_invalid)
            {
                return nullptr;
            }

            return &m_lib.forwarders()[m_index];
        }

        bool valid() const noexcept
        {
            if (m_index == k_invalid)
            {
                return false;
            }

            return desc()->OffsetModuleName != 0;
        }

        const char* libName() const noexcept
        {
            return reinterpret_cast<const char*>(m_lib.dirBase()) + desc()->OffsetModuleName;
        }

        unsigned int timestamp() const noexcept
        {
            return desc()->TimeDateStamp;
        }

        bool equals(const ForwarderEntry& entry) const noexcept
        {
            if (&m_lib != &entry.m_lib)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return index() == entry.index();
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_index;
            }
        }
    };

    using ForwarderIterator = GenericIterator<ForwarderEntry>;


    class LibEntry
    {
    private:
        const Pe& m_pe;
        const typename DirBoundImports::Type* const m_dirBase;
        const typename DirBoundImports::Type* m_desc;

    public:
        LibEntry(const Pe& pe, const typename DirBoundImports::Type* const desc) noexcept
            : m_pe(pe)
            , m_dirBase(desc)
            , m_desc(desc)
        {
        }

        const Pe& pe() const noexcept
        {
            return m_pe;
        }

        const typename DirBoundImports::Type* dirBase() const noexcept
        {
            return m_dirBase;
        }

        const typename DirBoundImports::Type* desc() const noexcept
        {
            return m_desc;
        }

        bool valid() const noexcept
        {
            return m_desc && m_desc->OffsetModuleName;
        }

        bool empty() const noexcept
        {
            return !valid() || !forwardersCount();
        }

        const char* libName() const noexcept
        {
            const auto offset = desc()->OffsetModuleName;
            if (!offset)
            {
                return nullptr;
            }

            return reinterpret_cast<const char*>(m_dirBase) + offset;
        }

        unsigned int forwardersCount() const noexcept
        {
            return desc()->NumberOfModuleForwarderRefs;
        }

        const typename GenericTypes::BoundForwarderRef* forwarders() const noexcept
        {
            if (empty())
            {
                return nullptr;
            }

            return reinterpret_cast<const typename GenericTypes::BoundForwarderRef*>(desc() + 1);
        }

        bool equals(const LibEntry& entry) const noexcept
        {
            if (&m_pe != &entry.m_pe)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return desc() == entry.desc();
        }

        void step() noexcept
        {
            if (valid())
            {
                m_desc = reinterpret_cast<const typename DirBoundImports::Type*>(reinterpret_cast<const unsigned char*>(forwarders()) + forwardersCount() * sizeof(typename GenericTypes::BoundForwarderRef));
            }
        }

        ForwarderIterator begin() const noexcept
        {
            if (empty())
            {
                return end();
            }

            return ForwarderIterator(*this, 0);
        }

        ForwarderIterator end() const noexcept
        {
            return ForwarderIterator(*this, ForwarderEntry::k_invalid);
        }
    };

    using LibIterator = GenericIterator<LibEntry>;

private:
    const Pe& m_pe;

public:
    explicit BoundImports(const Pe& pe) noexcept : m_pe(pe)
    {
    }

    const Pe& pe() const noexcept
    {
        return m_pe;
    }

    const typename DirBoundImports::Type* desc() const noexcept
    {
        return m_pe.dir<DirBoundImports>();
    }

    bool valid() const noexcept
    {
        return desc() != nullptr;
    }

    bool empty() const noexcept
    {
        const auto* const impDesc = desc();
        return !impDesc || !impDesc->OffsetModuleName;
    }

    LibIterator begin() const noexcept
    {
        return LibIterator(m_pe, desc());
    }

    LibIterator end() const noexcept
    {
        return LibIterator(m_pe, nullptr);
    }
};



class Exports
{
public:
    class FuncEntry
    {
    private:
        const Exports& m_exports;
        const typename GenericTypes::Eat* const m_eat;
        const Rva* const m_names;
        const Ordinal* const m_ordinals;
        unsigned int m_index;

    public:
        FuncEntry(const Exports& exports, const unsigned int index) noexcept
            : m_exports(exports)
            , m_eat(exports.pe().byRva<typename GenericTypes::Eat>(exports.desc()->AddressOfFunctions))
            , m_names(exports.pe().byRva<Rva>(exports.desc()->AddressOfNames))
            , m_ordinals(exports.pe().byRva<Ordinal>(exports.desc()->AddressOfNameOrdinals))
            , m_index(index)
        {
        }

        unsigned int index() const noexcept
        {
            return m_index;
        }

        const typename GenericTypes::Eat* eat() const noexcept
        {
            const auto ord = m_ordinals[m_index];
            return &m_eat[ord];
        }

        ExportType type() const noexcept
        {
            if (!valid())
            {
                return ExportType::unknown;
            }

            return !m_exports.contains(eat()->forwarderString)
                ? ExportType::exact
                : ExportType::forwarder;
        }

        const char* name() const noexcept
        {
            return m_exports.pe().byRva<char>(m_names[m_index]);
        }

        unsigned int ordinal() const noexcept
        {
            return m_ordinals[m_index];
        }

        const void* address() const noexcept
        {
            if (type() != ExportType::exact)
            {
                return nullptr;
            }

            return m_exports.pe().byRva<void>(eat()->address);
        }

        const char* forwarder() const noexcept
        {
            if (type() != ExportType::forwarder)
            {
                return nullptr;
            }

            return m_exports.pe().byRva<char>(eat()->forwarderString);
        }

        bool valid() const noexcept
        {
            return ordinal() < m_exports.count();
        }

        bool equals(const FuncEntry& entry) const noexcept
        {
            if (&m_exports != &entry.m_exports)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return index() == entry.index();
        }

        void step()
        {
            if (valid())
            {
                ++m_index;
            }
        }
    };

    using FuncIterator = GenericIterator<FuncEntry>;

private:
    const Pe& m_pe;
    const typename GenericTypes::ImgDataDir* const m_dir;
    const typename DirExports::Type* const m_desc;

public:
    explicit Exports(const Pe& pe) noexcept
        : m_pe(pe)
        , m_dir(pe.dir(DirExports::k_id))
        , m_desc(m_dir ? pe.byRva<typename DirExports::Type>(m_dir->VirtualAddress) : nullptr)
    {
    }

    const Pe& pe() const noexcept
    {
        return m_pe;
    }

    const Rva dirRva() const noexcept
    {
        return m_dir->VirtualAddress;
    }

    unsigned int dirSize() const noexcept
    {
        return m_dir->Size;
    }

    bool contains(Rva rva) const noexcept
    {
        return (rva >= dirRva()) && (rva < (dirRva() + dirSize()));
    }

    const typename DirExports::Type* desc() const noexcept
    {
        return m_desc;
    }

    bool valid() const noexcept
    {
        return desc() != nullptr;
    }

    bool empty() const noexcept
    {
        return count() == 0;
    }

    unsigned int count() const noexcept
    {
        return valid() ? desc()->NumberOfFunctions : 0;
    }

    const char* libName() const noexcept
    {
        const Rva rva = desc()->Name;
        return m_pe.byRva<char>(rva);
    }

    unsigned int base() const noexcept
    {
        return desc()->Base;
    }

    FuncIterator begin() const noexcept
    {
        if (!valid())
        {
            return end();
        }

        return FuncIterator(*this, 0);
    }

    FuncIterator end() const noexcept
    {
        return FuncIterator(*this, count());
    }
};



class Relocs
{
public:
    class PageEntry;

    class RelocEntry
    {
    public:
        static constexpr auto k_invalid = 0xFFFFFFFFu;

    private:
        const PageEntry& m_page;
        unsigned int m_index;

    public:
        RelocEntry(const PageEntry& page, const unsigned int index) noexcept : m_page(page), m_index(index)
        {
        }

        const PageEntry& page() const noexcept
        {
            return m_page;
        }

        const Reloc* reloc() const noexcept
        {
            const auto* const relocs = reinterpret_cast<const Reloc*>(m_page.desc() + 1);
            return &relocs[m_index];
        }

        const void* addr() const noexcept
        {
            return static_cast<const unsigned char*>(page().page()) + reloc()->offsetInPage;
        }

        bool valid() const noexcept
        {
            return m_index < m_page.count();
        }

        bool equals(const RelocEntry& entry) const noexcept
        {
            if (&m_page != &entry.m_page)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return m_index == entry.m_index;
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_index;
            }
        }
    };

    using RelocIterator = GenericIterator<RelocEntry>;

    class PageEntry
    {
    private:
        const Relocs& m_relocs;
        const typename DirRelocs::Type* m_entry;

    public:
        PageEntry(const Relocs& relocs, const typename DirRelocs::Type* entry) noexcept
            : m_relocs(relocs)
            , m_entry(entry)
        {
        }

        bool valid() const noexcept
        {
            return m_entry && m_entry->VirtualAddress && m_entry->SizeOfBlock;
        }

        const typename DirRelocs::Type* desc() const noexcept
        {
            return m_entry;
        }

        const void* page() const noexcept
        {
            return m_relocs.pe().byRva<void>(m_entry->VirtualAddress);
        }

        unsigned int count() const noexcept
        {
            return (m_entry->SizeOfBlock - sizeof(*m_entry)) / sizeof(Reloc); // Not including trailing empty element
        }

        bool equals(const PageEntry& entry) const noexcept
        {
            if (&m_relocs != &entry.m_relocs)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return desc() == entry.desc();
        }

        void step() noexcept
        {
            if (valid())
            {
                m_entry = reinterpret_cast<const typename DirRelocs::Type*>(
                    reinterpret_cast<const unsigned char*>(m_entry) + m_entry->SizeOfBlock
                );
            }
        }

        RelocIterator begin() const noexcept
        {
            if (!valid())
            {
                return end();
            }

            return RelocIterator(*this, 0);
        }

        RelocIterator end() const noexcept
        {
            return RelocIterator(*this, RelocEntry::k_invalid);
        }
    };

    using PageIterator = GenericIterator<PageEntry>;

private:
    const Pe& m_pe;
    const typename DirRelocs::Type* const m_table;

public:
    explicit Relocs(const Pe& pe) noexcept : m_pe(pe), m_table(pe.dir<DirRelocs>())
    {
    }

    const Pe& pe() const noexcept
    {
        return m_pe;
    }

    const DirRelocs::Type* table() const noexcept
    {
        return m_table;
    }

    bool valid() const noexcept
    {
        return m_pe.valid() && table();
    }

    PageIterator begin() const noexcept
    {
        if (!valid())
        {
            return end();
        }

        return PageIterator(*this, table());
    }

    PageIterator end() const noexcept
    {
        return PageIterator(*this, nullptr);
    }
};



class Exceptions
{
public:
    class RuntimeFunctionEntry
    {
    public:
        static constexpr auto k_invalid = 0xFFFFFFFFu;

    private:
        const Exceptions& m_exceptions;
        unsigned int m_index;

    public:
        explicit RuntimeFunctionEntry(const Exceptions& exceptions, const unsigned int index) noexcept
            : m_exceptions(exceptions)
            , m_index(index)
        {
        }

        const typename DirExceptions::Type* runtimeFunction() const noexcept
        {
            if (!valid())
            {
                return nullptr;
            }

            return &m_exceptions.runtimeFunctions()[m_index];
        }

        bool valid() const noexcept
        {
            return (m_index != k_invalid) && (m_exceptions.runtimeFunctions()[m_index].BeginAddress != 0);
        }

        bool equals(const RuntimeFunctionEntry& entry) const noexcept
        {
            if (&m_exceptions != &entry.m_exceptions)
            {
                return false;
            }

            const bool thisValid = valid();
            const bool objValid = entry.valid();
            if (thisValid != objValid)
            {
                // One is valid and another is invalid:
                return false;
            }

            if (!thisValid)
            {
                // Both are invalid:
                return true;
            }

            return runtimeFunction() == entry.runtimeFunction();
        }

        void step() noexcept
        {
            if (valid())
            {
                ++m_index;
            }
        }
    };

    using RuntimeFunctionIterator = GenericIterator<RuntimeFunctionEntry>;

private:
    const Pe& m_pe;
    const typename DirExceptions::Type* const m_runtimeFunctions;

public:
    explicit Exceptions(const Pe& pe) noexcept
        : m_pe(pe)
        , m_runtimeFunctions(pe.valid() ? pe.dir<DirExceptions>() : nullptr)
    {
    }

    const typename DirExceptions::Type* runtimeFunctions() const noexcept
    {
        return m_runtimeFunctions;
    }

    bool valid() const noexcept
    {
        return m_runtimeFunctions != nullptr;
    }

    RuntimeFunctionIterator begin() const noexcept
    {
        if (!valid())
        {
            return end();
        }

        return RuntimeFunctionIterator(*this, 0);
    }

    RuntimeFunctionIterator end() const noexcept
    {
        return RuntimeFunctionIterator(*this, RuntimeFunctionEntry::k_invalid);
    }
};



inline Sections Pe::sections() const noexcept
{
    if (m_arch == Arch::native)
    {
        const auto* const ntHdr = headers<Arch::native>().nt();
        const auto* const firstSection = IMAGE_FIRST_SECTION(ntHdr);
        const auto count = ntHdr->FileHeader.NumberOfSections;
        return Sections(firstSection, count);
    }
    else if (m_arch == Arch::inverse)
    {
        const auto* const ntHdr = headers<Arch::inverse>().nt();
        const auto* const firstSection = IMAGE_FIRST_SECTION(ntHdr);
        const auto count = ntHdr->FileHeader.NumberOfSections;
        return Sections(firstSection, count);
    }
    else
    {
        return Sections(nullptr, 0);
    }
}

inline Imports Pe::imports() const noexcept
{
    return Imports(*this);
}

inline DelayedImports Pe::delayedImports() const noexcept
{
    return DelayedImports(*this);
}

inline BoundImports Pe::boundImports() const noexcept
{
    return BoundImports(*this);
}

inline Exports Pe::exports() const noexcept
{
    return Exports(*this);
}

inline Relocs Pe::relocs() const noexcept
{
    return Relocs(*this);
}

inline Exceptions Pe::exceptions() const noexcept
{
    return Exceptions(*this);
}



} // namespace Pe