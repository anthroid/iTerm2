//
//  iTermGraphDeltaEncoder.m
//  iTerm2SharedARC
//
//  Created by George Nachman on 7/28/20.
//

#import "iTermGraphDeltaEncoder.h"

#import "NSArray+iTerm.h"
#import "iTermOrderedDictionary.h"

@implementation iTermGraphDeltaEncoder

- (instancetype)initWithPreviousRevision:(iTermEncoderGraphRecord * _Nullable)previousRevision {
    return [self initWithKey:@""
                  identifier:@""
                  generation:previousRevision.generation + 1
            previousRevision:previousRevision];
}

- (instancetype)initWithKey:(NSString *)key
                 identifier:(NSString *)identifier
                 generation:(NSInteger)generation
           previousRevision:(iTermEncoderGraphRecord * _Nullable)previousRevision {
    assert(identifier);
    self = [super initWithKey:key identifier:identifier generation:generation];
    if (self) {
        _previousRevision = previousRevision;
    }
    return self;
}

- (BOOL)encodeChildWithKey:(NSString *)key
                identifier:(NSString *)identifier
                generation:(NSInteger)generation
                     block:(BOOL (^ NS_NOESCAPE)(iTermGraphEncoder *subencoder))block {
    iTermEncoderGraphRecord *record = [_previousRevision childRecordWithKey:key
                                                                 identifier:identifier];
    if (!record) {
        // A wholly new key+identifier
        [super encodeChildWithKey:key identifier:identifier generation:generation block:block];
        return YES;
    }
    if (record.generation == generation) {
        // No change to generation
        [self encodeGraph:record];
        return YES;
    }
    // Same key+id, new generation.
    NSInteger realGeneration = generation;
    if (generation == iTermGenerationAlwaysEncode) {
        realGeneration = record.generation + 1;
    }
    assert(record.generation < generation);
    iTermGraphEncoder *encoder = [[iTermGraphDeltaEncoder alloc] initWithKey:key
                                                                  identifier:identifier
                                                                  generation:realGeneration
                                                            previousRevision:record];
    if (!block(encoder)) {
        return NO;
    }
    [self encodeGraph:encoder.record];
    return YES;
}

- (BOOL)enumerateRecords:(void (^)(iTermEncoderGraphRecord * _Nullable before,
                                   iTermEncoderGraphRecord * _Nullable after,
                                   NSNumber *parent,
                                   BOOL *stop))block {
    BOOL stop = NO;
    block(_previousRevision, self.record, @0, &stop);
    if (stop) {
        return NO;
    }
    return [self enumerateBefore:_previousRevision after:self.record parent:self.record.rowid block:block];
}

- (BOOL)enumerateBefore:(iTermEncoderGraphRecord *)preRecord
                  after:(iTermEncoderGraphRecord *)postRecord
                 parent:(NSNumber *)parent
                  block:(void (^)(iTermEncoderGraphRecord * _Nullable before,
                                  iTermEncoderGraphRecord * _Nullable after,
                                  NSNumber *parent,
                                  BOOL *stop))block {
    iTermOrderedDictionary<NSDictionary *, iTermEncoderGraphRecord *> *beforeDict =
    [iTermOrderedDictionary byMapping:preRecord.graphRecords block:^id _Nonnull(NSUInteger index,
                                                                                iTermEncoderGraphRecord * _Nonnull record) {
        return @{ @"key": record.key,
                  @"identifier": record.identifier };
    }];
    iTermOrderedDictionary<NSDictionary *, iTermEncoderGraphRecord *> *afterDict =
    [iTermOrderedDictionary byMapping:postRecord.graphRecords block:^id _Nonnull(NSUInteger index,
                                                                                iTermEncoderGraphRecord * _Nonnull record) {
        return @{ @"key": record.key,
                  @"identifier": record.identifier };
    }];
    __block BOOL ok = YES;
    void (^handle)(NSDictionary *,
                   iTermEncoderGraphRecord *,
                   BOOL *) = ^(NSDictionary *key,
                               iTermEncoderGraphRecord *record,
                               BOOL *stop) {
        iTermEncoderGraphRecord *before = beforeDict[key];
        iTermEncoderGraphRecord *after = afterDict[key];
        block(before, after, parent, stop);
        // Now recurse for their descendants.
        ok = [self enumerateBefore:before
                             after:after
                            parent:before ? before.rowid : after.rowid
                             block:block];
    };
    NSMutableSet<NSDictionary *> *seenKeys = [NSMutableSet set];
    [beforeDict.keys enumerateObjectsUsingBlock:^(NSDictionary * _Nonnull key, NSUInteger idx, BOOL * _Nonnull stop) {
        handle(key, beforeDict[key], stop);
        [seenKeys addObject:key];
    }];
    if (!ok) {
        return NO;
    }
    [afterDict.keys enumerateObjectsUsingBlock:^(NSDictionary * _Nonnull key, NSUInteger idx, BOOL * _Nonnull stop) {
        if ([seenKeys containsObject:key]) {
            return;
        }
        handle(key, afterDict[key], stop);
    }];
    return ok;
}


@end