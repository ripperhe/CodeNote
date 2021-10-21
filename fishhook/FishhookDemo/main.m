//
//  main.m
//  FishhookDemo
//
//  Created by ripper on 2021/9/22.
//

// main.m 文件

#import <Foundation/Foundation.h>
#import "fishhook.h"

/// 自定义动态库 变量
/// 貌似替换变量 fishhook 会报错，暂时先不管了，后面再看
extern char *global_var;
char *my_global_var = "my_world";

/// 系统动态库 方法
static void (*orgi_NSLog)(NSString *format, ...);
void my_NSLog(NSString *format, ...) {
    va_list args;
    va_start(args, format);
    NSString *string = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);

    printf("my_NSLog: %s\n", string.UTF8String);
}

int main(int argc, const char * argv[]) {
    @autoreleasepool {
//        printf("origin global_var: %s\n", global_var);
//        printf("my global_var: %s\n", my_global_var);
        NSLog(@"%s", global_var);

        struct rebinding rebind1 = {};
        rebind1.name = "NSLog";
        rebind1.replacement = my_NSLog;
        rebind1.replaced = (void *)&orgi_NSLog;
        
        struct rebinding rebind[1] = {rebind1};
        rebind_symbols(rebind, 1);
        
//        struct rebinding rebind2 = {};
//        rebind2.name = "global_var";
//        rebind2.replacement = my_global_var;
//        rebind2.replaced = NULL;
//        
//        struct rebinding rebind[2] = {rebind1, rebind2};
//        rebind_symbols(rebind, 2);
        
        NSLog(@"%s", global_var);
    }
    return 0;
}
