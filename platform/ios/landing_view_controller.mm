// SPDX-License-Identifier: GPL-2.0-or-later
#import "landing_view_controller.h"

@implementation LandingViewController

- (instancetype)init {
    if ((self = [super initWithNibName:nil bundle:nil])) {
        self.modalPresentationStyle = UIModalPresentationFullScreen;
        self.modalTransitionStyle = UIModalTransitionStyleCrossDissolve;
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UIButtonConfiguration* style = UIButtonConfiguration.filledButtonConfiguration;
    style.image = [UIImage systemImageNamed:@"map.fill"];
    style.imagePadding = 10;
    style.title = @"내비게이션";
    style.buttonSize = UIButtonConfigurationSizeLarge;
    style.cornerStyle = UIButtonConfigurationCornerStyleLarge;
    style.contentInsets = NSDirectionalEdgeInsetsMake(18, 28, 18, 28);
    UIButton* back = [UIButton buttonWithConfiguration:style primaryAction:nil];
    [back addTarget:self action:@selector(returnToGuest) forControlEvents:UIControlEventTouchUpInside];
    back.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:back];
    [NSLayoutConstraint activateConstraints:@[
        [back.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [back.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor]
    ]];
}

- (void)returnToGuest {
    [self dismissViewControllerAnimated:YES completion:nil];
}

@end
