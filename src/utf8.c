#include "katzip_internal.h"

static int invalid_name_component(const char *part, const char *end)
{
  if(end == part)
    return 1;
  return end - part == 2 && part[0] == '.' && part[1] == '.';
}

static const char *next_name_component(const char *part)
{
  const char *end = strchr(part, '/');
  if(!end)
    end = part + strlen(part);
  if(invalid_name_component(part, end))
    return NULL;
  return *end ? end + 1 : end;
}

int valid_name(const char *name)
{
  const char *part = name;
  if(!*name || *name == '/')
    return 0;
  while(*part)
  {
    part = next_name_component(part);
    if(!part)
      return 0;
  }
  return 1;
}

typedef struct {
  unsigned char low;
  unsigned char high;
  unsigned char mask;
  uint32_t minimum;
  int continuations;
} UTF8_LEAD;

static const UTF8_LEAD utf8_leads[] = {
  {0xc2, 0xdf, 0x1f, 0x80, 1},
  {0xe0, 0xef, 0x0f, 0x800, 2},
  {0xf0, 0xf4, 0x07, 0x10000, 3}
};

static const UTF8_LEAD *find_utf8_lead(unsigned char byte)
{
  size_t i;
  for(i = 0; i < ARRAY_N(utf8_leads); ++i)
    if(byte >= utf8_leads[i].low && byte <= utf8_leads[i].high)
      return &utf8_leads[i];
  return NULL;
}

static int read_utf8_continuations(const unsigned char **cursor,
  int count, uint32_t *codepoint)
{
  const unsigned char *p = *cursor;
  int i;
  for(i = 0; i < count; ++i)
  {
    if(!*p || (*p & 0xc0) != 0x80)
      return 0;
    *codepoint = (*codepoint << 6) | (*p++ & 0x3f);
  }
  *cursor = p;
  return 1;
}

static int valid_codepoint(uint32_t codepoint, uint32_t minimum)
{
  return codepoint >= minimum && codepoint <= 0x10ffff &&
    (codepoint < 0xd800 || codepoint > 0xdfff);
}

/* Advance only after validating the complete sequence. */
static int read_utf8_sequence(const unsigned char **cursor)
{
  const UTF8_LEAD *lead = find_utf8_lead(**cursor);
  const unsigned char *p = *cursor;
  uint32_t codepoint;
  if(!lead)
    return 0;
  codepoint = *p++ & lead->mask;
  if(!read_utf8_continuations(&p, lead->continuations, &codepoint))
    return 0;
  if(!valid_codepoint(codepoint, lead->minimum))
    return 0;
  *cursor = p;
  return 1;
}

/* Reject invalid leading bytes, incomplete sequences and invalid code points. */
int valid_utf8(const char *name)
{
  const unsigned char *p = (const unsigned char*)name;
  while(*p)
  {
    if(*p < 0x80)
    {
      ++p;
      continue;
    }
    if(!read_utf8_sequence(&p))
      return 0;
  }
  return 1;
}

/* Percentages use hundredths of a percent to avoid floating output drift. */
