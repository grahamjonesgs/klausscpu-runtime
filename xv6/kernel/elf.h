// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU // "\x7FELF" in little endian

// klausscpu: the toolchain emits ELF32 (32-bit addresses), so these are
// the Elf32_Ehdr and Elf32_Phdr layouts (note p_flags' position).

// File header
struct elfhdr {
  uint magic; // must equal ELF_MAGIC
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint entry;
  uint phoff;
  uint shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
};

// Program section header
struct proghdr {
  uint32 type;
  uint32 off;
  uint32 vaddr;
  uint32 paddr;
  uint32 filesz;
  uint32 memsz;
  uint32 flags;
  uint32 align;
};

// Values for Proghdr type
#define ELF_PROG_LOAD 1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC  1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ  4
