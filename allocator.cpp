#include "allocator.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>

// Helper function to find the most significant bit (MSB)
static inline int fls(std::size_t n) {
    if (n == 0) return -1;
    int pos = 0;
    while (n >>= 1) pos++;
    return pos;
}

// Helper function to find the first set bit (LSB)
static inline int ffs(std::uint32_t n) {
    if (n == 0) return -1;
    int pos = 0;
    while ((n & 1) == 0) {
        n >>= 1;
        pos++;
    }
    return pos;
}

TLSFAllocator::TLSFAllocator(std::size_t memoryPoolSize) : poolSize(memoryPoolSize) {
    initializeMemoryPool(memoryPoolSize);
}

TLSFAllocator::~TLSFAllocator() {
    if (memoryPool) {
        delete[] reinterpret_cast<char*>(memoryPool);
    }
}

void TLSFAllocator::initializeMemoryPool(std::size_t size) {
    // Allocate memory pool
    memoryPool = new char[size];

    // Initialize TLSF index
    index.fliBitmap = 0;
    for (int i = 0; i < FLI_SIZE; ++i) {
        index.sliBitmaps[i] = 0;
        for (int j = 0; j < SLI_SIZE; ++j) {
            index.freeLists[i][j] = nullptr;
        }
    }

    // Create initial free block
    FreeBlock* initialBlock = reinterpret_cast<FreeBlock*>(memoryPool);
    initialBlock->data = memoryPool;
    initialBlock->size = size;
    initialBlock->isFree = true;
    initialBlock->prevPhysBlock = nullptr;
    initialBlock->nextPhysBlock = nullptr;
    initialBlock->prevFree = nullptr;
    initialBlock->nextFree = nullptr;

    // Insert the initial free block into the free list
    insertFreeBlock(initialBlock);
}

void TLSFAllocator::mappingFunction(std::size_t size, int& fli, int& sli) {
    if (size < (1 << 5)) {
        fli = 0;
        sli = 0;
        return;
    }

    fli = fls(size);
    if (fli >= FLI_SIZE) {
        fli = FLI_SIZE - 1;
    }

    // Calculate SLI
    std::size_t range = 1ULL << fli;
    std::size_t divisions = std::min(range, static_cast<std::size_t>(SLI_SIZE));
    std::size_t offset = size - range;
    std::size_t slotSize = range / divisions;

    if (slotSize > 0) {
        sli = static_cast<int>(offset / slotSize);
        if (sli >= SLI_SIZE) sli = SLI_SIZE - 1;
    } else {
        sli = 0;
    }
}

void TLSFAllocator::insertFreeBlock(FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);

    // Insert at the head of the list
    block->prevFree = nullptr;
    block->nextFree = index.freeLists[fli][sli];

    if (index.freeLists[fli][sli]) {
        index.freeLists[fli][sli]->prevFree = block;
    }

    index.freeLists[fli][sli] = block;

    // Update bitmaps
    index.fliBitmap |= (1U << fli);
    index.sliBitmaps[fli] |= (1U << sli);
}

void TLSFAllocator::removeFreeBlock(FreeBlock* block) {
    int fli, sli;
    mappingFunction(block->size, fli, sli);

    // Remove from the free list
    if (block->prevFree) {
        block->prevFree->nextFree = block->nextFree;
    } else {
        index.freeLists[fli][sli] = block->nextFree;
    }

    if (block->nextFree) {
        block->nextFree->prevFree = block->prevFree;
    }

    // Update bitmaps if the list is now empty
    if (index.freeLists[fli][sli] == nullptr) {
        index.sliBitmaps[fli] &= ~(1U << sli);
        if (index.sliBitmaps[fli] == 0) {
            index.fliBitmap &= ~(1U << fli);
        }
    }
}

TLSFAllocator::FreeBlock* TLSFAllocator::findSuitableBlock(std::size_t size) {
    int fli, sli;
    mappingFunction(size, fli, sli);

    // Search in the current SLI level
    std::uint16_t slBitmap = index.sliBitmaps[fli] & (~0U << sli);

    if (slBitmap == 0) {
        // Search in higher FLI levels
        std::uint32_t flBitmap = index.fliBitmap & (~0U << (fli + 1));

        if (flBitmap == 0) {
            return nullptr; // No suitable block found
        }

        fli = ffs(flBitmap);
        slBitmap = index.sliBitmaps[fli];
    }

    sli = ffs(slBitmap);
    return index.freeLists[fli][sli];
}

void TLSFAllocator::splitBlock(FreeBlock* block, std::size_t size) {
    std::size_t remainingSize = block->size - size;

    // Minimum block size to avoid too small fragments
    const std::size_t minBlockSize = sizeof(FreeBlock);

    if (remainingSize >= minBlockSize) {
        // Create a new free block from the remaining space
        FreeBlock* newBlock = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<char*>(block->data) + size
        );

        newBlock->data = reinterpret_cast<void*>(newBlock);
        newBlock->size = remainingSize;
        newBlock->isFree = true;
        newBlock->prevPhysBlock = block;
        newBlock->nextPhysBlock = block->nextPhysBlock;
        newBlock->prevFree = nullptr;
        newBlock->nextFree = nullptr;

        if (block->nextPhysBlock) {
            block->nextPhysBlock->prevPhysBlock = newBlock;
        }

        block->nextPhysBlock = newBlock;
        block->size = size;

        // Insert the new free block into the free list
        insertFreeBlock(newBlock);
    }
}

void TLSFAllocator::mergeAdjacentFreeBlocks(FreeBlock* block) {
    // Merge with next block if it's free
    if (block->nextPhysBlock && block->nextPhysBlock->isFree) {
        FreeBlock* nextBlock = static_cast<FreeBlock*>(block->nextPhysBlock);

        // Remove next block from free list
        removeFreeBlock(nextBlock);

        // Merge
        block->size += nextBlock->size;
        block->nextPhysBlock = nextBlock->nextPhysBlock;

        if (block->nextPhysBlock) {
            block->nextPhysBlock->prevPhysBlock = block;
        }
    }

    // Merge with previous block if it's free
    if (block->prevPhysBlock && block->prevPhysBlock->isFree) {
        FreeBlock* prevBlock = static_cast<FreeBlock*>(block->prevPhysBlock);

        // Remove current block from free list (if it's already in)
        // We'll insert the merged block later

        // Remove previous block from free list
        removeFreeBlock(prevBlock);

        // Merge
        prevBlock->size += block->size;
        prevBlock->nextPhysBlock = block->nextPhysBlock;

        if (prevBlock->nextPhysBlock) {
            prevBlock->nextPhysBlock->prevPhysBlock = prevBlock;
        }

        block = prevBlock;
    }

    // Insert the merged block back into the free list
    insertFreeBlock(block);
}

void* TLSFAllocator::allocate(std::size_t size) {
    if (size == 0) return nullptr;

    // Align size to include header
    std::size_t totalSize = size + sizeof(BlockHeader);

    // Align to 8 bytes
    totalSize = (totalSize + 7) & ~7;

    // Find a suitable free block
    FreeBlock* block = findSuitableBlock(totalSize);

    if (!block) {
        return nullptr; // Out of memory
    }

    // Remove the block from the free list
    removeFreeBlock(block);

    // Split the block if it's too large
    splitBlock(block, totalSize);

    // Mark the block as used
    block->isFree = false;

    // Return pointer to the data area (after the header)
    return reinterpret_cast<char*>(block->data) + sizeof(BlockHeader);
}

void TLSFAllocator::deallocate(void* ptr) {
    if (!ptr) return;

    // Get the block header
    BlockHeader* blockHeader = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<char*>(ptr) - sizeof(BlockHeader)
    );

    // Mark the block as free
    blockHeader->isFree = true;

    // Convert to FreeBlock
    FreeBlock* block = static_cast<FreeBlock*>(blockHeader);
    block->prevFree = nullptr;
    block->nextFree = nullptr;

    // Merge adjacent free blocks
    mergeAdjacentFreeBlocks(block);
}

void* TLSFAllocator::getMemoryPoolStart() const {
    return memoryPool;
}

std::size_t TLSFAllocator::getMemoryPoolSize() const {
    return poolSize;
}

std::size_t TLSFAllocator::getMaxAvailableBlockSize() const {
    // Find the largest free block
    std::size_t maxSize = 0;

    for (int i = FLI_SIZE - 1; i >= 0; --i) {
        if (index.fliBitmap & (1U << i)) {
            for (int j = SLI_SIZE - 1; j >= 0; --j) {
                if (index.sliBitmaps[i] & (1U << j)) {
                    FreeBlock* block = index.freeLists[i][j];
                    while (block) {
                        if (block->size > maxSize) {
                            maxSize = block->size;
                        }
                        block = block->nextFree;
                    }
                }
            }
        }
    }

    // Subtract the header size
    if (maxSize >= sizeof(BlockHeader)) {
        return maxSize - sizeof(BlockHeader);
    }

    return 0;
}
