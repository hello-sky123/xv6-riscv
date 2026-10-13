/* 指定输出文件的目标架构是 RISC-V */
OUTPUT_ARCH( "riscv" )
/* 把符号 _entry 设为程序入口，链接器把它的地址写进 ELF 头的 Entry point address */
ENTRY( _entry )

SECTIONS
{
  /*
   * ensure that entry.S / _entry is at 0x80000000,
   * where qemu's -kernel jumps.
   * . 是位置计数器，表示“下一个东西放在哪一个地址”
   */
  . = 0x80000000;

  /* .text { ... } 定义一个输出段，花括号里每一行都是一个输入段模式
   * 链接器按顺序把匹配到的输入段放进去
   */
  .text : {
    kernel/entry.o(_entry)
    /* * 表示所有输入文件，这一行收集所有文件的 .text 和 .text.xxx 段，
     * 按文件在命令行上的顺序排列，OBJS 里第一个是 kernel/entry.o
     */
    *(.text .text.*)
    /* 位置计数器向上对齐到 4KB 边界 */
    . = ALIGN(0x1000);
    /* 定义符号，记下这一页的起点 */
    _trampoline = .;
    /* 把蹦床的汇编代码（trampsec）单独放进一个 4KB 对齐的内存页里 */
    *(trampsec)
    . = ALIGN(0x1000);
    /* 断言当前位置减去 _trampoline 是否等于 4KB */
    ASSERT(. - _trampoline == 0x1000, "error: trampoline larger than one page");
    /* 定义一个名为 etext 的符号，表示内核代码到此结束的地址，PROVIDE 表示只有程序引用了它、
     * 而且自己没有定义时，链接器才提供这个符号 */
    PROVIDE(etext = .);
  }

  .rodata : {
    /* 向上 16 字节对齐 */
    . = ALIGN(16);
    /* .srodata 是小数据的优化产物，默认是将不超过 8 字节的数据放进特殊的“small data”节 */
    *(.srodata .srodata.*) /* do not need to distinguish this from .rodata */
    . = ALIGN(16);
    *(.rodata .rodata.*)
  }

  .data : {
    . = ALIGN(16);
    *(.sdata .sdata.*) /* do not need to distinguish this from .data */
    . = ALIGN(16);
    *(.data .data.*)
  }

  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.*) /* do not need to distinguish this from .bss */
    . = ALIGN(16);
    *(.bss .bss.*)
  }

  PROVIDE(end = .);
}
