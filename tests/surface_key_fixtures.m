// CPU doubles only. Registry/monitor implementations are extracted at test time.
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#import <objc/runtime.h>
#include <assert.h>
#include <stdio.h>
#include "nvmtl_surface_key.h"
typedef struct { uint32_t sid, seed; } FakeSurface;
static uint32_t IOSurfaceGetID(FakeSurface *s) { return s->sid; }
static uint32_t IOSurfaceGetSeed(FakeSurface *s) { return s->seed; }
@interface NVMTLTexture : NSObject {
@public FakeSurface *_surf; NSUInteger _plane; BOOL _vramOn;
} @end
@implementation NVMTLTexture @end
@interface NVMTLSubmissionPart : NSObject { @public NSSet *surfaces; } @end
@implementation NVMTLSubmissionPart @end
@interface NVMTLCommandBuffer : NSObject { @public id _queue; NSArray *_parts; } @end
@implementation NVMTLCommandBuffer @end
@interface NVMTLPendingSurfaceWrite : NSObject {
@public __weak NVMTLCommandBuffer *buffer; __weak NVMTLTexture *texture; uint32_t seed;
} @end
@implementation NVMTLPendingSurfaceWrite @end
static NSMapTable *nvmtl_surface_writers;
static void nvmtl_ws_mark_ready(NVMTLCommandBuffer *cb) { (void)cb; }
