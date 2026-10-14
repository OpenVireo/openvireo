/*
 *  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#import "KPXCredentialProviderViewController.h"
#import "KPXAutofillClient.h"

typedef NS_ENUM(NSInteger, KPXListKind) {
    KPXListKindPassword,
    KPXListKindPasskey,
    KPXListKindOneTimeCode,
};

// Quick requests fail fast so the system can fall back to showing our interface
static const NSTimeInterval KPXQuickTimeout = 3;
static const NSTimeInterval KPXRequestTimeout = 15;
// The user unlocks the database in KeePassXC meanwhile
static const NSTimeInterval KPXUnlockTimeout = 300;
static const NSTimeInterval KPXLaunchTimeout = 20;

static NSString* const KPXCellIdentifier = @"KPXCredentialCell";

@interface KPXCredentialProviderViewController () <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate>

@property(nonatomic, strong) NSTextField* titleLabel;
@property(nonatomic, strong) NSTextField* messageLabel;
@property(nonatomic, strong) NSSearchField* searchField;
@property(nonatomic, strong) NSScrollView* scrollView;
@property(nonatomic, strong) NSTableView* tableView;
@property(nonatomic, strong) NSProgressIndicator* spinner;
@property(nonatomic, strong) NSButton* actionButton;
@property(nonatomic, strong) NSButton* cancelButton;
@property(nonatomic, strong) NSButton* primaryButton;

@property(nonatomic, copy) NSArray<ASCredentialServiceIdentifier*>* serviceIdentifiers;
@property(nonatomic) KPXListKind listKind;
// ASPasskeyCredentialRequestParameters, or a request to fill or register; kept untyped for macOS 11
@property(nonatomic, strong) id passkeyParameters;
@property(nonatomic, strong) id registrationRequest;
@property(nonatomic, copy) NSArray<NSDictionary*>* items;
@property(nonatomic, copy) NSArray<NSDictionary*>* filteredItems;

// What to run again once the database is unlocked
@property(nonatomic, copy) void (^retry)(void);
@property(nonatomic, copy) void (^primaryAction)(void);
@property(nonatomic, copy) void (^secondaryAction)(void);

@end

@implementation KPXCredentialProviderViewController

#pragma mark - View

- (void)loadView
{
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 460, 380)];

    self.titleLabel = [NSTextField labelWithString:@"KeePassXC"];
    self.titleLabel.font = [NSFont boldSystemFontOfSize:15];

    self.messageLabel = [NSTextField wrappingLabelWithString:@""];
    self.messageLabel.textColor = [NSColor secondaryLabelColor];

    self.searchField = [[NSSearchField alloc] init];
    self.searchField.placeholderString = NSLocalizedString(@"Search", nil);
    self.searchField.delegate = self;

    self.tableView = [[NSTableView alloc] init];
    NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@"credential"];
    column.resizingMask = NSTableColumnAutoresizingMask;
    [self.tableView addTableColumn:column];
    self.tableView.headerView = nil;
    self.tableView.rowHeight = 38;
    self.tableView.dataSource = self;
    self.tableView.delegate = self;
    self.tableView.target = self;
    self.tableView.doubleAction = @selector(fillSelected:);
    self.tableView.columnAutoresizingStyle = NSTableViewUniformColumnAutoresizingStyle;

    self.scrollView = [[NSScrollView alloc] init];
    self.scrollView.documentView = self.tableView;
    self.scrollView.hasVerticalScroller = YES;
    self.scrollView.borderType = NSBezelBorder;

    self.spinner = [[NSProgressIndicator alloc] init];
    self.spinner.style = NSProgressIndicatorStyleSpinning;
    self.spinner.controlSize = NSControlSizeSmall;
    self.spinner.displayedWhenStopped = NO;

    self.cancelButton = [NSButton buttonWithTitle:NSLocalizedString(@"Cancel", nil)
                                           target:self
                                           action:@selector(cancel:)];
    self.cancelButton.keyEquivalent = @"\e";
    self.actionButton = [NSButton buttonWithTitle:@"" target:self action:@selector(secondary:)];
    self.primaryButton = [NSButton buttonWithTitle:NSLocalizedString(@"Fill", nil)
                                            target:self
                                            action:@selector(primary:)];
    self.primaryButton.keyEquivalent = @"\r";

    NSStackView* buttons = [NSStackView stackViewWithViews:@[
        self.spinner, [NSView new], self.actionButton, self.cancelButton, self.primaryButton
    ]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    [buttons.views[1] setContentHuggingPriority:NSLayoutPriorityDefaultLow
                                 forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSStackView* stack = [NSStackView
        stackViewWithViews:@[ self.titleLabel, self.messageLabel, self.searchField, self.scrollView, buttons ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 10;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [view addSubview:stack];

    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:view.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:view.trailingAnchor constant:-20],
        [stack.topAnchor constraintEqualToAnchor:view.topAnchor constant:20],
        [stack.bottomAnchor constraintEqualToAnchor:view.bottomAnchor constant:-20],
        [self.messageLabel.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [self.searchField.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [self.scrollView.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
        [self.scrollView.heightAnchor constraintGreaterThanOrEqualToConstant:200],
        [buttons.widthAnchor constraintEqualToAnchor:stack.widthAnchor],
    ]];

    self.view = view;
    self.preferredContentSize = view.frame.size;
    [self showMessage:@"" title:@"KeePassXC"];
}

- (void)showListUI:(BOOL)list
{
    self.searchField.hidden = !list;
    self.scrollView.hidden = !list;
    self.primaryButton.hidden = !list;
    self.primaryButton.title = NSLocalizedString(@"Fill", nil);
    self.actionButton.hidden = YES;
    self.cancelButton.hidden = NO;
    self.primaryAction = nil;
    self.secondaryAction = nil;
    [self.spinner stopAnimation:nil];
}

- (void)showMessage:(NSString*)message title:(NSString*)title
{
    [self showListUI:NO];
    self.titleLabel.stringValue = title;
    self.messageLabel.stringValue = message;
}

- (void)showWaiting:(NSString*)message
{
    [self showMessage:message title:@"KeePassXC"];
    [self.spinner startAnimation:nil];
}

- (void)showMessage:(NSString*)message action:(NSString*)actionTitle perform:(void (^)(void))action
{
    [self showMessage:message title:@"KeePassXC"];
    self.actionButton.title = actionTitle;
    self.actionButton.hidden = NO;
    self.secondaryAction = action;
}

- (void)showPrimary:(NSString*)title perform:(void (^)(void))action
{
    self.primaryButton.title = title;
    self.primaryButton.hidden = NO;
    self.primaryAction = action;
}

#pragma mark - Buttons

- (void)cancel:(id)sender
{
    [self.extensionContext cancelRequestWithError:[NSError errorWithDomain:ASExtensionErrorDomain
                                                                      code:ASExtensionErrorCodeUserCanceled
                                                                  userInfo:nil]];
}

- (void)secondary:(id)sender
{
    if (self.secondaryAction) {
        self.secondaryAction();
    }
}

- (void)primary:(id)sender
{
    if (self.primaryAction) {
        self.primaryAction();
    } else {
        [self fillSelected:sender];
    }
}

#pragma mark - Talking to KeePassXC

- (void)failWithCode:(ASExtensionErrorCode)code
{
    [self.extensionContext cancelRequestWithError:[NSError errorWithDomain:ASExtensionErrorDomain
                                                                      code:code
                                                                  userInfo:nil]];
}

// Sends a request and hands a successful answer on; anything else shows what the user can do about it
- (void)send:(NSDictionary*)request timeout:(NSTimeInterval)timeout then:(void (^)(NSDictionary* response))success
{
    [KPXAutofillClient send:request
                    timeout:timeout
                 completion:^(NSDictionary* response, NSError* error) {
                     if (error) {
                         if (error.code == KPXAutofillErrorNotRunning) {
                             [self showNotRunning];
                         } else {
                             [self showMessage:error.localizedDescription title:@"KeePassXC"];
                         }
                         return;
                     }
                     NSString* failure = [KPXAutofillClient errorOfResponse:response];
                     if (!failure) {
                         success(response);
                     } else if ([failure isEqualToString:@"locked"]) {
                         [self showLocked];
                     } else if ([failure isEqualToString:@"disabled"]) {
                         [self showMessage:NSLocalizedString(@"AutoFill is turned off in KeePassXC. Turn it on in "
                                                             @"KeePassXC under Settings, AutoFill.",
                                                             nil)
                                     title:@"KeePassXC"];
                     } else if ([failure isEqualToString:@"notFound"]) {
                         [self showMessage:NSLocalizedString(@"KeePassXC has no matching entry in its unlocked "
                                                             @"databases.",
                                                             nil)
                                     title:@"KeePassXC"];
                     } else if ([failure isEqualToString:@"canceled"]) {
                         [self showLocked];
                     } else {
                         [self showMessage:NSLocalizedString(@"KeePassXC could not complete the request.", nil)
                                     title:@"KeePassXC"];
                     }
                 }];
}

- (void)showLocked
{
    __weak typeof(self) weakSelf = self;
    [self showMessage:NSLocalizedString(@"Your KeePassXC database is locked.", nil)
               action:NSLocalizedString(@"Unlock in KeePassXC", nil)
              perform:^{
                  [weakSelf unlock];
              }];
}

- (void)showNotRunning
{
    __weak typeof(self) weakSelf = self;
    [self showMessage:NSLocalizedString(@"KeePassXC is not running.", nil)
               action:NSLocalizedString(@"Open KeePassXC", nil)
              perform:^{
                  [weakSelf launchAndUnlock];
              }];
}

- (void)unlock
{
    [self showWaiting:NSLocalizedString(@"Unlock your database in KeePassXC to continue.", nil)];
    __weak typeof(self) weakSelf = self;
    [self send:@{@"type" : @"requestUnlock"}
        timeout:KPXUnlockTimeout
           then:^(__unused NSDictionary* response) {
               if (weakSelf.retry) {
                   weakSelf.retry();
               }
           }];
}

- (void)launchAndUnlock
{
    [self showWaiting:NSLocalizedString(@"Opening KeePassXC…", nil)];
    __weak typeof(self) weakSelf = self;
    [KPXAutofillClient launchKeePassXC:^(BOOL launched) {
        if (!launched) {
            [weakSelf showMessage:NSLocalizedString(@"Could not open KeePassXC. Open it yourself, then try again.", nil)
                            title:@"KeePassXC"];
            return;
        }
        [weakSelf waitForAppUntil:[NSDate dateWithTimeIntervalSinceNow:KPXLaunchTimeout]];
    }];
}

// A freshly launched app takes a moment before it listens
- (void)waitForAppUntil:(NSDate*)deadline
{
    __weak typeof(self) weakSelf = self;
    [KPXAutofillClient send:@{@"type" : @"status"}
                    timeout:KPXQuickTimeout
                 completion:^(__unused NSDictionary* response, NSError* error) {
                     if (!error) {
                         [weakSelf unlock];
                     } else if ([deadline timeIntervalSinceNow] > 0) {
                         dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
                                        dispatch_get_main_queue(),
                                        ^{
                                            [weakSelf waitForAppUntil:deadline];
                                        });
                     } else {
                         [weakSelf showNotRunning];
                     }
                 }];
}

#pragma mark - Credential list

- (void)prepareCredentialListForServiceIdentifiers:(NSArray<ASCredentialServiceIdentifier*>*)serviceIdentifiers
{
    self.serviceIdentifiers = serviceIdentifiers;
    self.listKind = KPXListKindPassword;
    [self loadList];
}

- (void)prepareCredentialListForServiceIdentifiers:(NSArray<ASCredentialServiceIdentifier*>*)serviceIdentifiers
                                 requestParameters:(ASPasskeyCredentialRequestParameters*)requestParameters
    API_AVAILABLE(macos(14.0))
{
    self.serviceIdentifiers = serviceIdentifiers;
    self.passkeyParameters = requestParameters;
    self.listKind = KPXListKindPasskey;
    [self loadList];
}

- (void)prepareOneTimeCodeCredentialListForServiceIdentifiers:
    (NSArray<ASCredentialServiceIdentifier*>*)serviceIdentifiers API_AVAILABLE(macos(15.0))
{
    self.serviceIdentifiers = serviceIdentifiers;
    self.listKind = KPXListKindOneTimeCode;
    [self loadList];
}

- (NSArray<NSDictionary*>*)serviceIdentifierValues
{
    NSMutableArray* values = [NSMutableArray array];
    for (ASCredentialServiceIdentifier* identifier in self.serviceIdentifiers) {
        NSString* type = identifier.type == ASCredentialServiceIdentifierTypeURL ? @"url" : @"domain";
        [values addObject:@{@"type" : type, @"value" : identifier.identifier}];
    }
    return values;
}

- (void)loadList
{
    __weak typeof(self) weakSelf = self;
    self.retry = ^{
        [weakSelf loadList];
    };
    [self showWaiting:NSLocalizedString(@"Looking for matching entries…", nil)];

    NSMutableDictionary* request = [@{
        @"type" : @"listCredentials",
        @"serviceIdentifiers" : [self serviceIdentifierValues],
    } mutableCopy];
    switch (self.listKind) {
    case KPXListKindPassword:
        request[@"kind"] = @"password";
        break;
    case KPXListKindOneTimeCode:
        request[@"kind"] = @"oneTimeCode";
        break;
    case KPXListKindPasskey:
        request[@"kind"] = @"passkey";
        if (@available(macOS 14.0, *)) {
            request[@"rpId"] = ((ASPasskeyCredentialRequestParameters*)self.passkeyParameters).relyingPartyIdentifier;
        }
        break;
    }

    [self send:request
        timeout:KPXRequestTimeout
           then:^(NSDictionary* response) {
               NSArray* items = response[@"items"];
               // A site that asks for a passkey may still take a password
               if (weakSelf.listKind == KPXListKindPasskey && items.count == 0) {
                   weakSelf.listKind = KPXListKindPassword;
                   [weakSelf loadList];
                   return;
               }
               [weakSelf showItems:items];
           }];
}

- (void)showItems:(NSArray<NSDictionary*>*)items
{
    self.items = items;
    [self showListUI:YES];
    NSString* service = self.serviceIdentifiers.firstObject.identifier;
    self.titleLabel.stringValue = service.length > 0
                                      ? [NSString stringWithFormat:NSLocalizedString(@"Entries for %@", nil), service]
                                      : NSLocalizedString(@"KeePassXC entries", nil);
    self.messageLabel.stringValue =
        items.count == 0 ? NSLocalizedString(@"Your unlocked databases have no entries to fill.", nil) : @"";
    if (items.count == 0 && self.serviceIdentifiers.count > 0) {
        // Offer every entry when none matches the site
        self.serviceIdentifiers = @[];
        [self loadList];
        return;
    }
    [self applyFilter];
    [self.view.window makeFirstResponder:self.searchField];
}

- (void)applyFilter
{
    NSString* query = self.searchField.stringValue;
    if (query.length == 0) {
        self.filteredItems = self.items;
    } else {
        NSPredicate* predicate =
            [NSPredicate predicateWithFormat:@"title CONTAINS[cd] %@ OR username CONTAINS[cd] %@ OR url CONTAINS[cd] %@",
                                             query,
                                             query,
                                             query];
        self.filteredItems = [self.items filteredArrayUsingPredicate:predicate];
    }
    [self.tableView reloadData];
    if (self.filteredItems.count > 0) {
        [self.tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    }
    self.primaryButton.enabled = self.filteredItems.count > 0;
}

- (void)controlTextDidChange:(NSNotification*)notification
{
    [self applyFilter];
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)commandSelector
{
    // Arrow keys in the search field move through the list
    if (commandSelector == @selector(moveDown:) || commandSelector == @selector(moveUp:)) {
        const NSInteger step = commandSelector == @selector(moveDown:) ? 1 : -1;
        const NSInteger row = MAX(0, MIN((NSInteger)self.filteredItems.count - 1, self.tableView.selectedRow + step));
        [self.tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
        [self.tableView scrollRowToVisible:row];
        return YES;
    }
    return NO;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
    return (NSInteger)self.filteredItems.count;
}

- (NSView*)tableView:(NSTableView*)tableView viewForTableColumn:(NSTableColumn*)tableColumn row:(NSInteger)row
{
    NSTableCellView* cell = [tableView makeViewWithIdentifier:KPXCellIdentifier owner:self];
    if (!cell) {
        cell = [[NSTableCellView alloc] init];
        cell.identifier = KPXCellIdentifier;
        NSTextField* text = [NSTextField labelWithString:@""];
        text.translatesAutoresizingMaskIntoConstraints = NO;
        text.lineBreakMode = NSLineBreakByTruncatingTail;
        [cell addSubview:text];
        cell.textField = text;
        [NSLayoutConstraint activateConstraints:@[
            [text.leadingAnchor constraintEqualToAnchor:cell.leadingAnchor constant:6],
            [text.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor constant:-6],
            [text.centerYAnchor constraintEqualToAnchor:cell.centerYAnchor],
        ]];
    }

    NSDictionary* item = self.filteredItems[(NSUInteger)row];
    NSString* title = [item[@"title"] length] > 0 ? item[@"title"] : NSLocalizedString(@"(no title)", nil);
    NSMutableAttributedString* text = [[NSMutableAttributedString alloc]
        initWithString:title
            attributes:@{NSFontAttributeName : [NSFont systemFontOfSize:NSFont.systemFontSize]}];
    if ([item[@"username"] length] > 0) {
        [text appendAttributedString:[[NSAttributedString alloc]
                                         initWithString:[@"\n" stringByAppendingString:item[@"username"]]
                                             attributes:@{
                                                 NSFontAttributeName : [NSFont systemFontOfSize:NSFont.smallSystemFontSize],
                                                 NSForegroundColorAttributeName : [NSColor secondaryLabelColor],
                                             }]];
    }
    cell.textField.attributedStringValue = text;
    cell.textField.maximumNumberOfLines = 2;
    return cell;
}

- (void)fillSelected:(id)sender
{
    const NSInteger row = self.tableView.selectedRow;
    if (row < 0 || row >= (NSInteger)self.filteredItems.count) {
        return;
    }
    NSDictionary* item = self.filteredItems[(NSUInteger)row];
    NSString* recordId = item[@"recordId"];

    switch (self.listKind) {
    case KPXListKindPassword:
        [self fillPasswordForRecord:recordId];
        break;
    case KPXListKindOneTimeCode:
        if (@available(macOS 15.0, *)) {
            [self fillOneTimeCodeForRecord:recordId];
        }
        break;
    case KPXListKindPasskey:
        if (@available(macOS 14.0, *)) {
            ASPasskeyCredentialRequestParameters* parameters = self.passkeyParameters;
            [self assertPasskeyForRecord:recordId
                            credentialId:nil
                                    rpId:parameters.relyingPartyIdentifier
                          clientDataHash:parameters.clientDataHash];
        }
        break;
    }
}

#pragma mark - Filling

- (void)fillPasswordForRecord:(NSString*)recordId
{
    __weak typeof(self) weakSelf = self;
    self.retry = ^{
        [weakSelf fillPasswordForRecord:recordId];
    };
    [self showWaiting:NSLocalizedString(@"Filling…", nil)];
    [self send:@{@"type" : @"getCredential", @"recordId" : recordId ?: @""}
        timeout:KPXRequestTimeout
           then:^(NSDictionary* response) {
               ASPasswordCredential* credential = [ASPasswordCredential credentialWithUser:response[@"username"]
                                                                                  password:response[@"password"]];
               [weakSelf.extensionContext completeRequestWithSelectedCredential:credential completionHandler:nil];
           }];
}

- (void)fillOneTimeCodeForRecord:(NSString*)recordId API_AVAILABLE(macos(15.0))
{
    __weak typeof(self) weakSelf = self;
    self.retry = ^{
        [weakSelf fillOneTimeCodeForRecord:recordId];
    };
    [self showWaiting:NSLocalizedString(@"Filling…", nil)];
    [self send:@{@"type" : @"getOneTimeCode", @"recordId" : recordId ?: @""}
        timeout:KPXRequestTimeout
           then:^(NSDictionary* response) {
               ASOneTimeCodeCredential* credential = [ASOneTimeCodeCredential credentialWithCode:response[@"code"]];
               [weakSelf.extensionContext completeOneTimeCodeRequestWithSelectedCredential:credential
                                                                         completionHandler:nil];
           }];
}

- (NSDictionary*)passkeyAssertRequest:(NSString*)recordId
                         credentialId:(NSData*)credentialId
                                 rpId:(NSString*)rpId
                       clientDataHash:(NSData*)clientDataHash
{
    NSMutableDictionary* request = [@{
        @"type" : @"passkeyAssert",
        @"rpId" : rpId ?: @"",
        @"clientDataHash" : [clientDataHash base64EncodedStringWithOptions:0] ?: @"",
    } mutableCopy];
    if (recordId.length > 0) {
        request[@"recordId"] = recordId;
    }
    if (credentialId.length > 0) {
        request[@"credentialId"] = [credentialId base64EncodedStringWithOptions:0];
    }
    return request;
}

- (ASPasskeyAssertionCredential*)assertionFromResponse:(NSDictionary*)response
                                                  rpId:(NSString*)rpId
                                        clientDataHash:(NSData*)clientDataHash API_AVAILABLE(macos(14.0))
{
    return [[ASPasskeyAssertionCredential alloc]
        initWithUserHandle:[[NSData alloc] initWithBase64EncodedString:response[@"userHandle"] options:0]
              relyingParty:rpId
                 signature:[[NSData alloc] initWithBase64EncodedString:response[@"signature"] options:0]
            clientDataHash:clientDataHash
         authenticatorData:[[NSData alloc] initWithBase64EncodedString:response[@"authenticatorData"] options:0]
              credentialID:[[NSData alloc] initWithBase64EncodedString:response[@"credentialId"] options:0]];
}

- (void)assertPasskeyForRecord:(NSString*)recordId
                  credentialId:(NSData*)credentialId
                          rpId:(NSString*)rpId
                clientDataHash:(NSData*)clientDataHash API_AVAILABLE(macos(14.0))
{
    __weak typeof(self) weakSelf = self;
    self.retry = ^{
        [weakSelf assertPasskeyForRecord:recordId credentialId:credentialId rpId:rpId clientDataHash:clientDataHash];
    };
    [self showWaiting:NSLocalizedString(@"Signing in with your passkey…", nil)];
    [self send:[self passkeyAssertRequest:recordId credentialId:credentialId rpId:rpId clientDataHash:clientDataHash]
        timeout:KPXRequestTimeout
           then:^(NSDictionary* response) {
               [weakSelf.extensionContext
                   completeAssertionRequestWithSelectedPasskeyCredential:[weakSelf assertionFromResponse:response
                                                                                                    rpId:rpId
                                                                                          clientDataHash:clientDataHash]
                                                       completionHandler:nil];
           }];
}

#pragma mark - Filling without the interface

// Without an interface there is nothing to show, so every failure goes back to the system
- (void)quickSend:(NSDictionary*)request then:(void (^)(NSDictionary* response))success
{
    [KPXAutofillClient send:request
                    timeout:KPXQuickTimeout
                 completion:^(NSDictionary* response, NSError* error) {
                     NSString* failure = error ? nil : [KPXAutofillClient errorOfResponse:response];
                     if (!error && !failure) {
                         success(response);
                     } else if ([failure isEqualToString:@"notFound"]) {
                         [self failWithCode:ASExtensionErrorCodeCredentialIdentityNotFound];
                     } else {
                         // Locked, not running or slow: the system shows our interface to sort it out
                         [self failWithCode:ASExtensionErrorCodeUserInteractionRequired];
                     }
                 }];
}

- (void)provideCredentialWithoutUserInteractionForIdentity:(ASPasswordCredentialIdentity*)credentialIdentity
{
    __weak typeof(self) weakSelf = self;
    [self quickSend:@{@"type" : @"getCredential", @"recordId" : credentialIdentity.recordIdentifier ?: @""}
               then:^(NSDictionary* response) {
                   [weakSelf.extensionContext
                       completeRequestWithSelectedCredential:[ASPasswordCredential credentialWithUser:response[@"username"]
                                                                                             password:response[@"password"]]
                                           completionHandler:nil];
               }];
}

- (void)provideCredentialWithoutUserInteractionForRequest:(id<ASCredentialRequest>)credentialRequest
    API_AVAILABLE(macos(14.0))
{
    __weak typeof(self) weakSelf = self;
    if ([credentialRequest isKindOfClass:[ASPasskeyCredentialRequest class]]) {
        ASPasskeyCredentialRequest* request = (ASPasskeyCredentialRequest*)credentialRequest;
        ASPasskeyCredentialIdentity* identity = (ASPasskeyCredentialIdentity*)request.credentialIdentity;
        NSString* rpId = identity.relyingPartyIdentifier;
        NSData* clientDataHash = request.clientDataHash;
        [self quickSend:[self passkeyAssertRequest:identity.recordIdentifier
                                      credentialId:identity.credentialID
                                              rpId:rpId
                                    clientDataHash:clientDataHash]
                   then:^(NSDictionary* response) {
                       [weakSelf.extensionContext
                           completeAssertionRequestWithSelectedPasskeyCredential:[weakSelf assertionFromResponse:response
                                                                                                            rpId:rpId
                                                                                                  clientDataHash:clientDataHash]
                                                               completionHandler:nil];
                   }];
        return;
    }

    if (@available(macOS 15.0, *)) {
        if ([credentialRequest isKindOfClass:[ASOneTimeCodeCredentialRequest class]]) {
            [self quickSend:@{
                @"type" : @"getOneTimeCode",
                @"recordId" : credentialRequest.credentialIdentity.recordIdentifier ?: @""
            }
                       then:^(NSDictionary* response) {
                           [weakSelf.extensionContext
                               completeOneTimeCodeRequestWithSelectedCredential:[ASOneTimeCodeCredential
                                                                                    credentialWithCode:response[@"code"]]
                                                              completionHandler:nil];
                       }];
            return;
        }
    }

    [self quickSend:@{
        @"type" : @"getCredential",
        @"recordId" : credentialRequest.credentialIdentity.recordIdentifier ?: @""
    }
               then:^(NSDictionary* response) {
                   [weakSelf.extensionContext
                       completeRequestWithSelectedCredential:[ASPasswordCredential credentialWithUser:response[@"username"]
                                                                                             password:response[@"password"]]
                                           completionHandler:nil];
               }];
}

#pragma mark - Filling after unlocking

// The system asks for this interface when filling without it failed, which usually means the database is locked
- (void)prepareInterfaceToProvideCredentialForIdentity:(ASPasswordCredentialIdentity*)credentialIdentity
{
    [self fillPasswordForRecord:credentialIdentity.recordIdentifier];
}

- (void)prepareInterfaceToProvideCredentialForRequest:(id<ASCredentialRequest>)credentialRequest
    API_AVAILABLE(macos(14.0))
{
    if ([credentialRequest isKindOfClass:[ASPasskeyCredentialRequest class]]) {
        ASPasskeyCredentialRequest* request = (ASPasskeyCredentialRequest*)credentialRequest;
        ASPasskeyCredentialIdentity* identity = (ASPasskeyCredentialIdentity*)request.credentialIdentity;
        [self assertPasskeyForRecord:identity.recordIdentifier
                        credentialId:identity.credentialID
                                rpId:identity.relyingPartyIdentifier
                      clientDataHash:request.clientDataHash];
        return;
    }
    if (@available(macOS 15.0, *)) {
        if ([credentialRequest isKindOfClass:[ASOneTimeCodeCredentialRequest class]]) {
            [self fillOneTimeCodeForRecord:credentialRequest.credentialIdentity.recordIdentifier];
            return;
        }
    }
    [self fillPasswordForRecord:credentialRequest.credentialIdentity.recordIdentifier];
}

#pragma mark - Passkey registration

- (void)prepareInterfaceForPasskeyRegistration:(id<ASCredentialRequest>)registrationRequest API_AVAILABLE(macos(14.0))
{
    if (![registrationRequest isKindOfClass:[ASPasskeyCredentialRequest class]]) {
        [self failWithCode:ASExtensionErrorCodeFailed];
        return;
    }
    self.registrationRequest = registrationRequest;
    [self confirmRegistration];
}

- (void)confirmRegistration API_AVAILABLE(macos(14.0))
{
    ASPasskeyCredentialRequest* request = self.registrationRequest;
    ASPasskeyCredentialIdentity* identity = (ASPasskeyCredentialIdentity*)request.credentialIdentity;
    // The user confirms here; KeePassXC adds the entry without asking again
    [self showMessage:[NSString stringWithFormat:NSLocalizedString(@"Save a passkey for “%@” on %@ in KeePassXC?", nil),
                                                 identity.userName,
                                                 identity.relyingPartyIdentifier]
                title:NSLocalizedString(@"Save Passkey", nil)];
    __weak typeof(self) weakSelf = self;
    [self showPrimary:NSLocalizedString(@"Save Passkey", nil)
              perform:^{
                  [weakSelf registerPasskey];
              }];
}

- (void)registerPasskey API_AVAILABLE(macos(14.0))
{
    ASPasskeyCredentialRequest* request = self.registrationRequest;
    ASPasskeyCredentialIdentity* identity = (ASPasskeyCredentialIdentity*)request.credentialIdentity;
    NSString* rpId = identity.relyingPartyIdentifier;
    NSData* clientDataHash = request.clientDataHash;

    __weak typeof(self) weakSelf = self;
    self.retry = ^{
        [weakSelf registerPasskey];
    };
    [self showWaiting:NSLocalizedString(@"Saving the passkey…", nil)];
    [self send:@{
        @"type" : @"passkeyRegister",
        @"rpId" : rpId ?: @"",
        @"userName" : identity.userName ?: @"",
        @"userHandle" : [identity.userHandle base64EncodedStringWithOptions:0] ?: @"",
        @"clientDataHash" : [clientDataHash base64EncodedStringWithOptions:0] ?: @"",
        @"algorithms" : request.supportedAlgorithms ?: @[],
    }
        timeout:KPXRequestTimeout
           then:^(NSDictionary* response) {
               ASPasskeyRegistrationCredential* credential = [[ASPasskeyRegistrationCredential alloc]
                   initWithRelyingParty:rpId
                         clientDataHash:clientDataHash
                           credentialID:[[NSData alloc] initWithBase64EncodedString:response[@"credentialId"] options:0]
                      attestationObject:[[NSData alloc] initWithBase64EncodedString:response[@"attestationObject"]
                                                                            options:0]];
               [weakSelf.extensionContext completeRegistrationRequestWithSelectedPasskeyCredential:credential
                                                                                 completionHandler:nil];
           }];
}

#pragma mark - Configuration

- (void)prepareInterfaceForExtensionConfiguration
{
    [self showMessage:NSLocalizedString(@"KeePassXC fills passwords, passkeys and one-time codes from your unlocked "
                                        @"databases. Turn on AutoFill in KeePassXC under Settings, AutoFill too.",
                                        nil)
                title:NSLocalizedString(@"KeePassXC AutoFill is on", nil)];
    self.cancelButton.hidden = YES;
    __weak typeof(self) weakSelf = self;
    [self showPrimary:NSLocalizedString(@"Done", nil)
              perform:^{
                  [weakSelf.extensionContext completeExtensionConfigurationRequest];
              }];
}

@end
