//
//  ViewController.m
//  KSCrashDemo
//
//  Created by ripper on 2021/10/29.
//

#import "ViewController.h"

@interface ViewController ()

@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    // Do any additional setup after loading the view.
}

- (IBAction)buttonClicked:(id)sender {
    NSLog(@"点击按钮");
    
    // 1
//    char* ptr = (char*)-1;
//    *ptr = 10;
    
    
    // 2
    NSArray *array = @[@1, @2];
    NSLog(@"%@", array[2]);
}

@end
