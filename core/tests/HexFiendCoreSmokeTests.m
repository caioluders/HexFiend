#import <HexFiend/HexFiendCore.h>
#include <unistd.h>

static void require(BOOL condition, NSString *message) {
    if (!condition) {
        NSLog(@"%@", message);
        exit(EXIT_FAILURE);
    }
}

static NSData *dataFromByteArray(HFByteArray *byteArray) {
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)[byteArray length]];
    [byteArray copyBytes:[data mutableBytes] range:HFRangeMake(0, [byteArray length])];
    return data;
}

int main(void) {
    @autoreleasepool {
        NSData *initialData = [@"0123456789" dataUsingEncoding:NSASCIIStringEncoding];
        HFFullMemoryByteSlice *initialSlice = [[HFFullMemoryByteSlice alloc] initWithData:initialData];
        HFBTreeByteArray *byteArray = [[HFBTreeByteArray alloc] initWithByteSlice:initialSlice];

        require([byteArray length] == 10, @"initial length mismatch");
        require([dataFromByteArray(byteArray) isEqualToData:initialData], @"initial data mismatch");

        NSData *insertData = [@"abcd" dataUsingEncoding:NSASCIIStringEncoding];
        HFFullMemoryByteSlice *insertSlice = [[HFFullMemoryByteSlice alloc] initWithData:insertData];
        [byteArray insertByteSlice:insertSlice inRange:HFRangeMake(5, 0)];

        NSData *expectedInsert = [@"01234abcd56789" dataUsingEncoding:NSASCIIStringEncoding];
        require([byteArray length] == [expectedInsert length], @"insert length mismatch");
        require([dataFromByteArray(byteArray) isEqualToData:expectedInsert], @"insert data mismatch");

        [byteArray deleteBytesInRange:HFRangeMake(3, 4)];
        NSData *expectedDelete = [@"012cd56789" dataUsingEncoding:NSASCIIStringEncoding];
        require([byteArray length] == [expectedDelete length], @"delete length mismatch");
        require([dataFromByteArray(byteArray) isEqualToData:expectedDelete], @"delete data mismatch");

        HFBTreeByteArray *needle = [[HFBTreeByteArray alloc] initWithByteSlice:[[HFFullMemoryByteSlice alloc] initWithData:[@"cd5" dataUsingEncoding:NSASCIIStringEncoding]]];
        unsigned long long found = [byteArray indexOfBytesEqualToBytes:needle inRange:HFRangeMake(0, [byteArray length]) searchingForwards:YES trackingProgress:nil];
        require(found == 3, @"search result mismatch");

        char pathTemplate[] = "/tmp/hexfiend-core-smoke-XXXXXX";
        int fd = mkstemp(pathTemplate);
        require(fd >= 0, @"mkstemp failed");
        close(fd);

        NSString *path = [NSString stringWithUTF8String:pathTemplate];
        NSURL *url = [NSURL fileURLWithPath:path];
        NSError *error = nil;
        require([byteArray writeToFile:url trackingProgress:nil error:&error], [NSString stringWithFormat:@"write failed: %@", error]);
        NSData *writtenData = [NSData dataWithContentsOfFile:path];
        require([writtenData isEqualToData:expectedDelete], @"written file mismatch");
        unlink(pathTemplate);
    }
    return EXIT_SUCCESS;
}
