#include "iniparse.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#ifdef __GNUC__
#include <strings.h>
#define stricmp strcasecmp
#define strnicmp strncasecmp
#endif

static int find_next_char(const char* byteBuf, const int maxBytes, const char toFind)
{
	if (byteBuf == NULL || maxBytes == 0)
		return -1;

	for (int i=0; i<maxBytes; i++)
	{
		if (byteBuf[i] == toFind)
			return i;
	}

	return -1;
}

static bool is_space(const char theChar)
{
	return (theChar == 0x20) || (theChar >= 0x09 && theChar <= 0x0d);
}

static int trim_trailing_whitespace(char* textBytes, const int maxLen)
{
	if (textBytes == NULL || maxLen == 0)
		return maxLen;

	int realLen = maxLen;
	for (int i=maxLen-1; i>=0; i--)
	{
		if (is_space(textBytes[i]))
		{
			textBytes[i] = 0;
			realLen = i;
		}
		else
			break;
	}

	return realLen;
}

static char* my_strdup(const char* srcString, AllocatorFunc allocator)
{
	if (srcString == NULL)
		return NULL;

	size_t srcLen = strlen(srcString);
	char* dstString = (char*)allocator(srcLen+1);
	memcpy(dstString, srcString, srcLen+1);
	return dstString;
}

IniParsedInfo_t parse_memloader_ini(char* iniBytes, const int numBytes, AllocatorFunc allocator, ErrPrintFunc printer)
{
	IniParsedInfo_t out;
	out.loads = NULL;
	out.copies = NULL;
	out.boots = NULL;

	IniLoadSectionNode_t* currLoadNode = NULL;
	IniCopySectionNode_t* currCopyNode = NULL;
	IniBootSectionNode_t* currBootNode = NULL;

	int currLine = -1;
	int currPos = 0;	
	int bytesRemaining = numBytes;
	while (bytesRemaining > 0)
	{
		char* const lineStart = &iniBytes[currPos];

		char* currBytes = lineStart;
		int lineLength = find_next_char(currBytes, bytesRemaining, '\n');
		if (lineLength >= 0)
		{
			currBytes[lineLength] = 0;
			bytesRemaining -= lineLength+1;
			currPos += lineLength+1;
		}
		else
		{
			lineLength = (int)bytesRemaining;
			bytesRemaining = 0;
			currPos += lineLength;
		}
		currLine++;

		//skip leading space
		while (is_space(*currBytes) && lineLength > 0)
		{
			currBytes++;
			lineLength--;
		}

		//trim comments
		{
			int semiColonPos = find_next_char(currBytes, lineLength, ';');
			if (semiColonPos >= 0)
			{
				currBytes[semiColonPos] = 0;
				lineLength = semiColonPos;
			}
		}
		
		lineLength = trim_trailing_whitespace(currBytes, lineLength);
		if (lineLength < 1)
			continue;

		//new section start
		if (*currBytes == '[')
		{
			currBytes++;
			lineLength--;

			//skip leading space
			while (is_space(*currBytes) && lineLength > 0)
			{
				currBytes++;
				lineLength--;
			}

			int colonPos = find_next_char(currBytes, lineLength, ':');
			if (colonPos < 0)
			{
				printer("Cannot find : separator in section name '%s' on line %d, skipping\n", currBytes, currLine);
				continue;
			}
			currBytes[colonPos] = 0;

			//left side processing
			char* leftSide = currBytes;
			int leftSideLen = colonPos;		
			leftSideLen = trim_trailing_whitespace(leftSide, leftSideLen);

			//right side processing
			char* rightSide = currBytes+colonPos+1;
			int rightSideLen = lineLength-colonPos-1;

			while (is_space(*rightSide) && rightSideLen > 0)
			{
				rightSide++;
				rightSideLen--;
			}
			int rightSideClosingPos = find_next_char(rightSide, rightSideLen, ']');
			if (rightSideClosingPos < 0)
				printer("No closing ] found for section '%s' on line %d, behaving as if it was there\n", rightSide, currLine);
			else
			{
				rightSide[rightSideClosingPos] = 0;
				rightSideLen = rightSideClosingPos;
			}
			rightSideLen = trim_trailing_whitespace(rightSide, rightSideLen);

			currLoadNode = NULL;
			currCopyNode = NULL;
			currBootNode = NULL;
			if (strnicmp(leftSide, "load", leftSideLen) == 0)
			{
				if (out.loads == NULL)
				{
					out.loads = allocator(sizeof(IniLoadSectionNode_t));
					currLoadNode = out.loads;
					memset(currLoadNode, 0, sizeof(IniLoadSectionNode_t));
				}
				else
				{
					IniLoadSectionNode_t* currNode = out.loads;
					while (currNode->next != NULL) 
					{ 
						if (stricmp(currNode->curr.sectname, rightSide) == 0)
						{
							currLoadNode = currNode;
							break;
						}
						currNode = currNode->next; 
					}
					if (currLoadNode == NULL)
					{
						currNode->next = allocator(sizeof(IniLoadSectionNode_t));
						currLoadNode = currNode->next;
						memset(currLoadNode, 0, sizeof(IniLoadSectionNode_t));
					}
				}

				if (currLoadNode->curr.sectname == NULL)
					currLoadNode->curr.sectname = my_strdup(rightSide, allocator);
			}
			else if (strnicmp(leftSide, "copy", leftSideLen) == 0)
			{
				if (out.copies == NULL)
				{
					out.copies = allocator(sizeof(IniCopySectionNode_t));
					currCopyNode = out.copies;
					memset(currCopyNode, 0, sizeof(IniCopySectionNode_t));
				}
				else
				{
					IniCopySectionNode_t* currNode = out.copies;
					while (currNode->next != NULL)
					{
						if (stricmp(currNode->curr.sectname, rightSide) == 0)
						{
							currCopyNode = currNode;
							break;
						}
						currNode = currNode->next;
					}
					if (currCopyNode == NULL)
					{
						currNode->next = allocator(sizeof(IniCopySectionNode_t));
						currCopyNode = currNode->next;
						memset(currCopyNode, 0, sizeof(IniCopySectionNode_t));
					}
				}

				if (currCopyNode->curr.sectname == NULL)
					currCopyNode->curr.sectname = my_strdup(rightSide, allocator);
			}
			else if (strnicmp(leftSide, "boot", leftSideLen) == 0)
			{
				if (out.boots == NULL)
				{
					out.boots = allocator(sizeof(IniBootSectionNode_t));
					currBootNode = out.boots;
					memset(currBootNode, 0, sizeof(IniBootSectionNode_t));
				}
				else
				{
					IniBootSectionNode_t* currNode = out.boots;
					while (currNode->next != NULL)
					{
						if (stricmp(currNode->curr.sectname, rightSide) == 0)
						{
							currBootNode = currNode;
							break;
						}
						currNode = currNode->next;
					}
					if (currBootNode == NULL)
					{
						currNode->next = allocator(sizeof(IniBootSectionNode_t));
						currBootNode = currNode->next;
						memset(currBootNode, 0, sizeof(IniBootSectionNode_t));
					}
				}
				if (currBootNode->curr.sectname == NULL)
					currBootNode->curr.sectname = my_strdup(rightSide, allocator);
			}
			else
			{
				printer("Unknown section type '%s' on line %d\n", rightSide, currLine);
				continue;
			}
		}
		else //key=value
		{
			int equalsPos = find_next_char(currBytes, lineLength, '=');
			if (equalsPos < 0)
			{
				printer("Cannot find = separator in kv pair '%s' on line %d, skipping\n", currBytes, currLine);
				continue;
			}
			currBytes[equalsPos] = 0;

			//left side processing
			char* leftSide = currBytes;
			int leftSideLen = equalsPos;
			leftSideLen = trim_trailing_whitespace(leftSide, leftSideLen);

			//right side processing
			char* rightSide = currBytes+equalsPos+1;
			int rightSideLen = lineLength-equalsPos-1;
			while (is_space(*rightSide) && rightSideLen > 0)
			{
				rightSide++;
				rightSideLen--;
			}

			if (currLoadNode != NULL)
			{
				enum { KEY_INPUTFILE, KEY_SKIPBYTES, KEY_COUNTBYTES, KEY_DSTADDR, KEY_COUNT };
				static const char* keyNames[KEY_COUNT] = { "if", "skip", "count", "dst" };

				int currKey;
				for (currKey=0; currKey<KEY_COUNT; currKey++)
				{
					if (strnicmp(leftSide, keyNames[currKey], leftSideLen) == 0)
						break;
				}

				if (currKey == KEY_COUNT)
				{
					printer("Unknown key '%s' for LOAD section on line %d, skipping\n", leftSide, currLine);
					continue;
				}
				else if (currKey == KEY_INPUTFILE)
					currLoadNode->curr.filename = my_strdup(rightSide,allocator);
				else
				{
					char* outPos = NULL;
					uint32_t theValue = strtoul(rightSide, &outPos, 0);
					if (outPos == NULL || outPos == rightSide)
						printer("Invalid value '%s' for LOAD section key '%s' on line %d, skipping\n", rightSide, leftSide, currLine);
					else if (currKey == KEY_SKIPBYTES)
						currLoadNode->curr.skip = theValue;
					else if (currKey == KEY_COUNTBYTES)
						currLoadNode->curr.count = theValue;
					else if (currKey == KEY_DSTADDR)
						currLoadNode->curr.dst = theValue;
				}
			}
			else if (currCopyNode != NULL)
			{
				enum { KEY_COMPTYPE, KEY_SRCADDR, KEY_SRCLEN, KEY_DSTADDR, KEY_DSTLEN, KEY_COUNT };
				static const char* keyNames[KEY_COUNT] ={ "type", "src", "srclen", "dst", "dstlen" };
				int currKey;
				for (currKey=0; currKey<KEY_COUNT; currKey++)
				{
					if (strnicmp(leftSide, keyNames[currKey], leftSideLen) == 0)
						break;
				}

				if (currKey == KEY_COUNT)
				{
					printer("Unknown key '%s' for COPY section on line %d, skipping\n", leftSide, currLine);
					continue;
				}
				else
				{
					char* outPos = NULL;
					uint32_t theValue = strtoul(rightSide, &outPos, 0);
					if (outPos == NULL || outPos == rightSide)
						printer("Invalid value '%s' for COPY section key '%s' on line %d, skipping\n", rightSide, leftSide, currLine);
					else if (currKey == KEY_COMPTYPE)
						currCopyNode->curr.compType = theValue;
					else if (currKey == KEY_SRCADDR)
						currCopyNode->curr.src = theValue;
					else if (currKey == KEY_SRCLEN)
						currCopyNode->curr.srclen = theValue;
					else if (currKey == KEY_DSTADDR)
						currCopyNode->curr.dst = theValue;
					else if (currKey == KEY_DSTLEN)
						currCopyNode->curr.dstlen = theValue;
				}
			}
			else if (currBootNode != NULL)
			{
				enum { KEY_PCADDR, KEY_COUNT };
				static const char* keyNames[KEY_COUNT] ={ "pc" };
				int currKey;
				for (currKey=0; currKey<KEY_COUNT; currKey++)
				{
					if (strnicmp(leftSide, keyNames[currKey], leftSideLen) == 0)
						break;
				}

				if (currKey == KEY_COUNT)
				{
					printer("Unknown key '%s' for BOOT section on line %d, skipping\n", leftSide, currLine);
					continue;
				}
				else
				{
					char* outPos = NULL;
					uint32_t theValue = strtoul(rightSide, &outPos, 0);
					if (outPos == NULL || outPos == rightSide)
						printer("Invalid value '%s' for BOOT section key '%s' on line %d, skipping\n", rightSide, leftSide, currLine);
					else if (currKey == KEY_PCADDR)
						currBootNode->curr.pc = theValue;
				}
			}
			else
			{
				printer("Key '%s' outside of recognized section on line %d, skipping\n", leftSide, currLine);
				continue;
			}
		}
	}

	return out;
}

void free_memloader_info(IniParsedInfo_t* infoPtr, DeallocatorFunc deallocator)
{
	if (infoPtr->loads != NULL)
	{
		IniLoadSectionNode_t* currNode = infoPtr->loads;
		while (currNode != NULL)
		{
			IniLoadSectionNode_t* freeMe = currNode;
			currNode = currNode->next;
			if (freeMe->curr.sectname != NULL)
				deallocator(freeMe->curr.sectname);	
			if (freeMe->curr.filename != NULL)
				deallocator(freeMe->curr.filename);	

			deallocator(freeMe);
		}
		infoPtr->loads = NULL;
	}

	if (infoPtr->copies != NULL)
	{
		IniCopySectionNode_t* currNode = infoPtr->copies;
		while (currNode != NULL)
		{
			IniCopySectionNode_t* freeMe = currNode;
			currNode = currNode->next;
			if (freeMe->curr.sectname != NULL)
				deallocator(freeMe->curr.sectname);	

			deallocator(freeMe);
		}
		infoPtr->copies = NULL;
	}

	if (infoPtr->boots != NULL)
	{
		IniBootSectionNode_t* currNode = infoPtr->boots;
		while (currNode != NULL)
		{
			IniBootSectionNode_t* freeMe = currNode;
			currNode = currNode->next;
			if (freeMe->curr.sectname != NULL)
				deallocator(freeMe->curr.sectname);	

			deallocator(freeMe);
		}
		infoPtr->boots = NULL;
	}
}
