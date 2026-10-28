// Lists windows so they can be captured with `screencapture -l <CGWindowID>` even while
// another window (e.g. the DeepSeek Harness GUI) covers them completely.
//
//   ./winid                 # every window: id pid owner layer bounds title
//   ./winid <pid>           # only that process
//   ./winid <substring>     # only owners whose name contains it (case-insensitive)
//
// Then:  screencapture -x -l <id> /tmp/shot.png
//
// NOTE (learned the hard way): `kCGWindowListOptionOnScreenOnly` often reports *nothing*
// for a game window that is merely occluded - the list comes back empty even though the
// window is alive.  `kCGWindowListOptionAll` always sees it, which is why this tool uses
// `All` and leaves the "is it visible" question to the capture itself.  Filtering by PID
// is the reliable way to find the main window (the small 0x0 / 1470x33 / 64x64 entries
// are SDL and IME helper windows).
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdlib.h>

int main(int argc, const char** argv)
{
	@autoreleasepool
	{
		const char* arg = argc > 1 ? argv[1] : NULL;
		const int wantPid = arg != NULL ? atoi(arg) : -1;
		NSString* needle = (arg != NULL && wantPid <= 0)
			? [NSString stringWithUTF8String:arg]
			: nil;

		CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);

		for (NSDictionary* w in (__bridge NSArray*)list)
		{
			const int pid = [w[(id)kCGWindowOwnerPID] intValue];
			NSString* owner = w[(id)kCGWindowOwnerName];

			if (wantPid > 0 && pid != wantPid) continue;
			if (needle != nil && (owner == nil
				|| [owner rangeOfString:needle options:NSCaseInsensitiveSearch].location == NSNotFound))
			{
				continue;
			}

			NSDictionary* b = w[(id)kCGWindowBounds];
			NSString* title = w[(id)kCGWindowName];

			printf("%d\t%d\t%s\t%d\t%.0f,%.0f %.0fx%.0f\t%s\n",
				[w[(id)kCGWindowNumber] intValue],
				pid,
				owner != nil ? owner.UTF8String : "?",
				[w[(id)kCGWindowLayer] intValue],
				[b[@"X"] doubleValue], [b[@"Y"] doubleValue],
				[b[@"Width"] doubleValue], [b[@"Height"] doubleValue],
				title != nil ? title.UTF8String : "");
		}

		if (list != NULL) CFRelease(list);
	}
	return 0;
}
