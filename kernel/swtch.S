# Context switch
#
#   void swtch(struct context *old, struct context *new);
# 
# Save current registers in old. Load from new.	


# 告诉汇编器和链接器，swtch是一个全局符号，可以被其他文件引用，以 . 开头的指令是给汇编器的元指令
.globl swtch
# 按 RISC-V 调用约定，函数的第一个参数通过 a0 寄存器传递，第二个参数通过 a1 寄存器传递
# sd 指令（Store Doubleword）：将 64 位的寄存器数据，存入内存；第一部分 14 条指令把当前 CPU 里的 14
# 个寄存器（属于当前线程，可能是某个进程，也可能是 scheduler），依次备份到 a0 所指的内存空间里
# （p->context 或 c->context）
swtch:
        sd ra, 0(a0)
        sd sp, 8(a0)
        sd s0, 16(a0)
        sd s1, 24(a0)
        sd s2, 32(a0)
        sd s3, 40(a0)
        sd s4, 48(a0)
        sd s5, 56(a0)
        sd s6, 64(a0)
        sd s7, 72(a0)
        sd s8, 80(a0)
        sd s9, 88(a0)
        sd s10, 96(a0)
        sd s11, 104(a0)

        ld ra, 0(a1)
        ld sp, 8(a1)
        ld s0, 16(a1)
        ld s1, 24(a1)
        ld s2, 32(a1)
        ld s3, 40(a1)
        ld s4, 48(a1)
        ld s5, 56(a1)
        ld s6, 64(a1)
        ld s7, 72(a1)
        ld s8, 80(a1)
        ld s9, 88(a1)
        ld s10, 96(a1)
        ld s11, 104(a1)
        
        ret

	
