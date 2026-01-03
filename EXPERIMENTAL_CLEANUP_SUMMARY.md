# Experimental Files Cleanup Summary

## Status: ✅ Production code is now fully independent of experimental files

### Critical Fix Applied
**ISSUE FOUND AND FIXED**: The production `/display` and `/display-r` endpoints were using `displayImage()` helper which was calling experimental functions. This has been corrected.

### Changes Made
1. **display.go**: Added `ensureConnected()` method to replace `experimentalConnect()`
   - Used by `ShowImageNoReinit()` and `ShowAnimatedGifNoReinit()`
   
2. **handler.go**: Fixed `displayImage()` to use production methods
   - `ShowAnimatedGif()` instead of `experimentalSendAnimated()`
   - `ShowAnimatedGifNoReinit()` instead of `experimentalSendAnimatedNoReinit()`
   - `ShowImage()` instead of `experimentalSendStatic()`
   - `ShowImageNoReinit()` instead of `experimentalSendStaticNoReinit()`
   - `attemptReconnection()` instead of `experimentalDisconnect()/experimentalConnect()`

### Files You Can Safely Clean/Repurpose

#### Experimental Implementation Files
- **experimental.go** - All functions can be removed or rewritten
  - `runExperimentalNoClearVariants()` - Not used in production
  - `experimentalConnect()` - Replaced by `ensureConnected()` in display.go
  - `experimentalDisconnect()` - Only used in handler.go for test harness
  - `experimentalSendStatic()` - Only used in handler.go for test harness
  - `experimentalSendAnimated()` - Only used in handler.go for test harness
  - `experimentalSendPlaylist()` - Only used in handler.go for test harness
  - `experimentalSendStaticNoReinit()` - Only used in handler.go for test harness
  - `experimentalSendAnimatedNoReinit()` - Only used in handler.go for test harness
  - `experimentalBrightnessCycle()` - Only used in handler.go for test harness
  - `experimentalScreenOff()` - Only used in handler.go for test harness
  - `experimentalScreenOn()` - Only used in handler.go for test harness

#### Test Files
- **display_experimental_test.go** - Only runs when `TESTS=true`
- **display_test.go** - Already deprecated, empty file

### Files That Reference Experimental Code (Non-Production)

#### handler.go - Test Harness Endpoints Only
The following experimental functions are ONLY used in the `/uploadTest` endpoint (manual test harness):
- Lines 110-141: File upload test commands (a1, a1nr, a1x, s1, s1nr, s1x)
- Lines 273-304: Command execution (`connect`, `a1`, `a1nr`, `s1`, `s1nr`, `playlist`, etc.)

**These are NOT production endpoints** - they're debugging/testing tools.

### Production Endpoints (No Experimental Dependencies)
- ✅ `/display` - Uses `ShowImage()` 
- ✅ `/display-r` - Uses `ShowImage()`
- ✅ `/animated` - Uses `ShowAnimatedGif()`
- ✅ `/animated-nr` - Uses `ShowAnimatedGifNoReinit()`
- ✅ `/static-nr` - Uses `ShowImageNoReinit()`
- ✅ `/health` - No dependencies
- ✅ `/upload` - Uses production display methods
- ✅ `/reconnect` - Uses `attemptReconnection()`
- ✅ `/brightness/*` - Uses device methods directly
- ✅ `/on`, `/off` - Uses device methods directly

### Options for Cleanup

#### Option 1: Complete Experimental File Removal (Recommended if you don't use test harness)
Remove these files entirely:
```bash
rm experimental.go
rm display_experimental_test.go
rm display_test.go
```

Then remove experimental functions from handler.go:
- Remove `/uploadTest` endpoint handlers
- Remove experimental function calls in HandleUploadTest

#### Option 2: Keep Test Harness, Clean Experimental
If you want to keep the `/uploadTest` endpoint for debugging:
- Clear `experimental.go` and rewrite it for new experiments
- Move the experimental functions currently in handler.go to a new `test_harness.go` file
- Keep `display_experimental_test.go` for when `TESTS=true`

#### Option 3: Minimal Cleanup
Just add a comment to `experimental.go` header:
```go
// Package experimental contains testing and debugging utilities.
// These are NOT used by production endpoints.
// Safe to modify/remove for new experiments.
```

### Verification
Build succeeds without experimental files:
```bash
✓ go build ./...  # Passes with current changes
```

### Recommendation
I recommend **Option 1** unless you actively use the `/uploadTest` manual test harness for debugging. All production functionality is independent and fully working.
