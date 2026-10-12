        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
# .section：指定接下来要放入哪个段，.text：段的名称，按照 elf 文件约定，用来存放可执行代码
.section .text
# 声明 _entry 是一个全局符号，其他文件可以引用它
.global _entry
# 标号，代表下一条指令的地址，也就是 0x80000000
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        # 把 stack0 的地址加载到 sp 寄存器中
        la sp, stack0
        # 把立即数 1024*4 加载到 a0 寄存器中
        li a0, 1024*4
        # 读取当前 hart 的编号，放进 a1 寄存器中
        csrr a1, mhartid
        # 栈向低地址方向增长，sp 要指向分给自己那块区域的顶端（addi 立即数加法）
        addi a1, a1, 1
        # 乘法，a0 = a0 * a1
        mul a0, a0, a1
        # sp = stack0 + ((hartid + 1) * 4096)
        add sp, sp, a0
        # jump to start() in start.c
        call start
spin:
        # 跳回自己，它是一个保护措施，正常情况下不会执行到这里
        j spin
