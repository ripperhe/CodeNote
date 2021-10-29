// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#import "fishhook.h"

#import <dlfcn.h>
#import <stdlib.h>
#import <string.h>
#import <sys/types.h>
#import <mach-o/dyld.h>
#import <mach-o/loader.h>
#import <mach-o/nlist.h>

#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

/// 重绑定信息条目
struct rebindings_entry {
    /// 重绑定信息数组
    struct rebinding *rebindings;
    /// 重绑定信息个数
    size_t rebindings_nel;
    /// 下一个重绑定信息条目
    struct rebindings_entry *next;
};

/// 重绑定信息条目链表头部
static struct rebindings_entry *_rebindings_head;


/// 准备重绑定信息条目，将新的条目塞到链表头部
/// @param rebindings_head 链表头部
/// @param rebindings 新的重绑定信息数组
/// @param nel 新的重绑定信息个数
static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                              struct rebinding rebindings[],
                              size_t nel) {
    // 为新的重绑定信息条目分配内存
    struct rebindings_entry *new_entry = malloc(sizeof(struct rebindings_entry));
    if (!new_entry) {
        return -1;
    }
    /// 为重绑定信息数组分配内存
    new_entry->rebindings = malloc(sizeof(struct rebinding) * nel);
    if (!new_entry->rebindings) {
        free(new_entry);
        return -1;
    }
    /// 给重绑定信息数组赋值
    memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
    /// 给重绑定信息个数赋值
    new_entry->rebindings_nel = nel;
    /// 将重绑定条目的下一个条目指向当前链表头
    new_entry->next = *rebindings_head;
    /// 将链表头指向新的创建的重绑定条目
    *rebindings_head = new_entry;
    return 0;
}

/// 实际重绑定某个 section 的对应符号
/// @param rebindings 重绑定信息
/// @param section 需要重绑定的 section
/// @param slide ALSR 偏移
/// @param symtab 符号表
/// @param strtab 字符串标
/// @param indirect_symtab 间接符号表
static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab,
                                           char *strtab,
                                           uint32_t *indirect_symtab) {
    // section 结构体的 reserved1 代表这个 section 的符号在 间接符号表(Indirect Symbol Table) 的起始的 index
    // indirect_symtab  K+ section->reserved1 相当于就是 indirect_symtab[section->reserved1]
    // 即 section 内第一个符号在间接符号表的地址
    // 然后间接符号表里面存的是都是索引，这个索引指的是对应的这个符号在符号表的索引
    // 所以由此就可以找到该 section 的所有符号
    uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
    // section 的数据段的真实地址 = slide(ASLR 偏移) + section addr
    // 实际上就对应 Lazy Symbol Pointers 和 Non-Lazy Symbol Pointers 真实的地址
    // 可以认为就是个数组，里面全是指针，这个区域是可读写的，改掉这里的地址，相当于就是改变了符号真实执行时的地址
    // ((uintptr_t)slide + section->addr) 是 section 对应的数据段的地址，加上 void * 就可以代表这个地址的指针
    // 然后这个数据段数组里面的数据又都是指针，所以再加一个 * 修饰，也就是二级指针 void **indirect_symbol_bindings
    // 分开来看
    // void *x = (void *)((uintptr_t)slide + section->addr);
    // void **indirect_symbol_bindings = (void *)x;
    void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
    // 遍历符号；(section size / 指针的 size) 得出 section 内符号个数
    for (uint i = 0; i < section->size / sizeof(void *); i++) {
        // i 代表当前 section 第 i 个，是个索引
        // indirect_symbol_indices[i] 可以获取到该符号在符号表的索引
        uint32_t symtab_index = indirect_symbol_indices[i];
        // 过滤掉部分特殊的索引
        // FIXME: hezongyi 这些特殊的索引是干嘛的？
        if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
            symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
            continue;
        }
        // symtab 是符号表，symtab[symtab_index] 可获取到该符号的结构体 nlist_64
        // symtab[symtab_index].n_un.n_strx 代表该符号的符号名在字符串表里面的偏移（下标、索引、index）
        uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
        // 字符串表地址 + 符号名偏移 = 符号名
        char *symbol_name = strtab + strtab_offset;
        // 获取当前重绑定条目头部
        struct rebindings_entry *cur = rebindings;
        // 如果存在重绑定条目
        while (cur) {
            // 遍历当前重绑定条目的所有信息
            for (uint j = 0; j < cur->rebindings_nel; j++) {
                // 判断符号名长度 > 1；并且符号名和需要重绑定的符号名相等
                // 注意，这里判断符号名相等的时候，用的是 &symbol_name[1]，下标从 1 开始
                // 本质上是因为 C 符号默认前面加个下划线 _;
                // e.g. NSLog 的符号名是 _NSLog
                if (strlen(symbol_name) > 1 &&
                    strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
                    // 如果重绑定信息中保留原始符号地址的二级指针不为 NULL
                    // 并且当前的符号地址不等于重绑定信息中新的符号地址
                    if (cur->rebindings[j].replaced != NULL &&
                        indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
                        // 记录当前符号地址到重绑定信息中保留原始符号地址的二级指针中
                        *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
                    }
                    // 将重绑定信息中新的符号地址赋值给当前符号地址
                    indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
                    // 跳出循环 while 循环
                    // 每次调用 rebind_symbols 方法的时候都会生成一个 rebindings_entry 结构体
                    // 每个 rebindings_entry 结构体里面可能会有多个 rebinding 结构体，每个代表一个函数替换
                    // 每次都会将新的 rebindings_entry 放到指针头部
                    // 从 rebindings_entry 头指针开始遍历，当前符号需要替换的信息，就将其替换
                    // 然后就跳出对 rebindings_entry 链表的 while 循环
                    // 然后通过外层的 for 循环看下一个符号是否需要替换
                    // 指导当前 section 的所有符号被遍历完毕
                    goto symbol_loop;
                }
            }
            // 将重绑定条目的 next 赋值给 cur，然后循环遍历下一个重绑定条目里面的所有信息
            cur = cur->next;
        }
        // 进入下一个符号的遍历
    symbol_loop:;
    }
}

/// 实际重绑定镜像符号
/// @param rebindings 重绑定信息条目头
/// @param header 镜像头地址
/// @param slide ASLR 偏移
static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide) {
    // dladdr 函数的作用是根据一个地址获取镜像的信息
    // 这里应该想要判断 header 地址对应到一个正确的镜像，如果有问题就返回了
    Dl_info info;
    if (dladdr(header, &info) == 0) {
        return;
    }
    
    segment_command_t *cur_seg_cmd;
    // LINKEDIT segment是link editor在链接时候创建生成的segment，这个段包含了符号表(symtab)、间接符号表(dysymtab)、字符串表(string table)等
    segment_command_t *linkedit_segment = NULL;
    // 符号表 command
    struct symtab_command* symtab_cmd = NULL;
    // 间接符号表 command；里面有间接符号表（Indirect Symbol Table）的信息，可以找到间接符号表
    struct dysymtab_command* dysymtab_cmd = NULL;
    
    // 镜像内存地址 + mach header = Load Commands 头部（第一个 segment command 头部）
    uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
    // 遍历所有 segment command
    for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
        // 获取当前 segment_command_t
        cur_seg_cmd = (segment_command_t *)cur;
        
        if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
            // 如果 segment command 为 LC_SEGMENT
            // 并且 segment name 为 __LINKEDIT
            if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
                // 保存 linkedit_segment
                linkedit_segment = cur_seg_cmd;
            }
        } else if (cur_seg_cmd->cmd == LC_SYMTAB) {
            // 如果 segment command 为 LC_SYMTAB
            // 保存 symtab_cmd
            symtab_cmd = (struct symtab_command*)cur_seg_cmd;
        } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
            // 如果 segment command 为 LC_DYSYMTAB
            // 保存 dysymtab_cmd
            dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
        }
    }
    
    // 如果其中有任何一个 segment command 没找到
    // 或者间接符号表条目个数为 0
    // 直接返回
    if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
        !dysymtab_cmd->nindirectsyms) {
        return;
    }
    
    // linkedit_segment->vmaddr 代表该 segment 虚拟内存地址
    // linkedit_segment->fileoff 代表该 segment 相对镜像头部的偏移
    // (linkedit_segment->vmaddr - linkedit_segment->fileoff) 则为虚拟的镜像头部（基地址）
    // 由于每个镜像都要加上 slide(ASLR 偏移)
    // 真实的基地址 = ASLR 偏移 + linkedit_segment 虚拟地址 - linkedit_segment 偏移
    // Find base symbol/string table addresses
    uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    // 符号表地址 = 基地址 + 符号表的偏移
    nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
    // 字符串表地址 = 基地址 + 字符串表偏移
    char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);
    
    // 间接符号表地址 = 基地址 + 间接符号表偏移
    // Get indirect symbol table (array of uint32_t indices into symbol table)
    uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);
    
    // 再次重置：镜像内存地址 + mach header = Load Commands 头部（第一个 segment command 头部）
    cur = (uintptr_t)header + sizeof(mach_header_t);
    // 再次遍历所有 segment command
    for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
        cur_seg_cmd = (segment_command_t *)cur;
        // 如果 segment command 为 LC_SEGMENT
        if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
            // 如果 segment name 不是 __DATA  或 __DATA_CONST，直接跳过
            if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
                strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0) {
                continue;
            }
            // 遍历当前 segment 所有 section
            for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
                // section header 的内容应该是跟在 segment command 后面的
                // 所以 (cur + sizeof(segment_command_t)) 就是第 0 个 section 的头部
                section_t *sect =
                (section_t *)(cur + sizeof(segment_command_t)) + j;
                
                // Lazy Symbol Pointers，延迟绑定的数据段
                // 延迟绑定，需要等到程序第一次使用的对应符号的时候才解析符号地址
                // 一般是模块外的函数放这里
                if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
                    perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
                }
                
                // FIXME: hezongyi 待确认
                // 全局偏移表（Global Offset Table，GOT），也称 Non-Lazy Symbol Pointers
                // 非延迟绑定，动态链接阶段，就寻找好所有数据符号的地址
                // 一般是模块外的全局变量的符号地址放这这里
                // 1. 编译的时候，GOT 里面是空的
                // 2. 程序装载的时候，dyld 会将镜像加载到内存里面，然后重定位，把 GOT 里面的值填好
                // 3. 当代码需要引用该全局变量时，可以通过GOT中相对应的项间接引用
                // 当成是个数组，里面全是对应符号的地址
                if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
                    perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
                }
            }
        }
    }
}

/// 重绑定镜像
/// @param header 镜像头地址
/// @param slide ASLR 偏移
static void _rebind_symbols_for_image(const struct mach_header *header,
                                      intptr_t slide) {
    /// 实际绑定镜像
    rebind_symbols_for_image(_rebindings_head, header, slide);
}

/// 仅冲绑定某个镜像的符号
/// @param header 镜像头地址
/// @param slide ASLR 偏移
/// @param rebindings 重绑定信息
/// @param rebindings_nel 重绑定信息个数
int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel) {
    // 定义重绑定条目
    struct rebindings_entry *rebindings_head = NULL;
    // 组装重绑定条目链表
    int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel);
    // 真正重绑定镜像内符号
    rebind_symbols_for_image(rebindings_head, header, slide);
    // 释放重绑定条目
    free(rebindings_head);
    return retval;
}

/// 重绑定所有镜像的符号
/// @param rebindings 重绑定信息数组
/// @param rebindings_nel 重绑定信息数组元素个数
int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
    // 将需要替换的 rebinding 变成链表，_rebindings_head 作为表头
    int retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);
    if (retval < 0) {
        return retval;
    }
    
    // If this was the first call, register callback for image additions (which is also invoked for
    // existing images, otherwise, just run on existing images
    if (!_rebindings_head->next) {
        // 如果是第一次进入这里，_rebindings_head->next 为 NULL
        // 下面这个方法干两件事
        // 1. 立即为系统中已经加载的所有镜像调用一次 _rebind_symbols_for_image 函数
        // 2. 以后有任何镜像被 load 的时候，为相应镜像调用 _rebind_symbols_for_image 函数
        _dyld_register_func_for_add_image(_rebind_symbols_for_image);
    } else {
        // 如果不是第一次进入，证明已经注册过 _dyld_register_func_for_add_image 方法
        // 此时只需要手动遍历所有镜像，然后调用 _rebind_symbols_for_image 函数
        uint32_t c = _dyld_image_count();
        for (uint32_t i = 0; i < c; i++) {
            // _dyld_get_image_header 获取到的是镜像加载地址
            // _dyld_get_image_vmaddr_slide 获取到的是镜像的 ASLR 偏移
            _rebind_symbols_for_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
        }
    }
    return retval;
}
