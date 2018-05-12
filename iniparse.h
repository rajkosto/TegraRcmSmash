#ifndef _INIPARSE_H_
#define _INIPARSE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IniLoadSection_s
{
	char* sectname;
	char* filename;
	uint32_t skip;
	uint32_t count;
	uint32_t dst;
} IniLoadSection_t;

typedef struct IniCopySection_s
{
	char* sectname;
	uint32_t compType;
	uint32_t src;
	uint32_t srclen;
	uint32_t dst;
	uint32_t dstlen;
} IniCopySection_t;

typedef struct IniBootSection_s
{
	char* sectname;
	uint32_t pc;
} IniBootSection_t;

typedef struct IniLoadSectionNode_s
{
	IniLoadSection_t curr;
	struct IniLoadSectionNode_s* next;
} IniLoadSectionNode_t;

typedef struct IniCopySectionNode_s
{
	IniCopySection_t curr;
	struct IniCopySectionNode_s* next;
} IniCopySectionNode_t;

typedef struct IniBootSectionNode_s
{
	IniBootSection_t curr;
	struct IniBootSectionNode_s* next;
} IniBootSectionNode_t;

typedef struct IniParsedInfo_s
{
	IniLoadSectionNode_t* loads;
	IniCopySectionNode_t* copies;
	IniBootSectionNode_t* boots;
} IniParsedInfo_t;

typedef void*(*AllocatorFunc)(size_t numBytes);
typedef int(*ErrPrintFunc)(const char* format, ...);
IniParsedInfo_t parse_memloader_ini(char* iniBytes, const int numBytes, AllocatorFunc allocator, ErrPrintFunc printer);

typedef void(*DeallocatorFunc)(void* allocBytes);
void free_memloader_info(IniParsedInfo_t* infoPtr, DeallocatorFunc deallocator);

#ifdef __cplusplus
}
#endif

#endif