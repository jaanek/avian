/* Copyright (c) 2008, Avian Contributors

   Permission to use, copy, modify, and/or distribute this software
   for any purpose with or without fee is hereby granted, provided
   that the above copyright notice and this permission notice appear
   in all copies.

   There is NO WARRANTY for this software.  See license.txt for
   details. */

#include "zlib-custom.h"
#include "system.h"
#include "tokenizer.h"
#include "finder.h"

using namespace vm;

namespace {

const bool DebugFind = false;

const char*
append(System* s, const char* a, const char* b, const char* c)
{
  unsigned al = strlen(a);
  unsigned bl = strlen(b);
  unsigned cl = strlen(c);
  char* p = static_cast<char*>(allocate(s, (al + bl + cl) + 1));
  memcpy(p, a, al);
  memcpy(p + al, b, bl);
  memcpy(p + al + bl, c, cl + 1);
  return p;
}

const char*
copy(System* s, const char* a)
{
  unsigned al = strlen(a);
  char* p = static_cast<char*>(allocate(s, al + 1));
  memcpy(p, a, al + 1);
  return p;
}

bool
equal(const void* a, unsigned al, const void* b, unsigned bl)
{
  if (al == bl) {
    return memcmp(a, b, al) == 0;
  } else {
    return false;
  }
}

class Element {
 public:
  class Iterator {
   public:
    virtual const char* next(unsigned* size) = 0;
    virtual void dispose() = 0;
  };

  Element(): next(0) { }

  virtual Iterator* iterator() = 0;
  virtual System::Region* find(const char* name) = 0;
  virtual bool exists(const char* name) = 0;
  virtual void dispose() = 0;

  Element* next;
};

class DirectoryElement: public Element {
 public:
  class Iterator: public Element::Iterator {
   public:
    Iterator(System* s, const char* name, unsigned skip):
      s(s), name(name), skip(skip), directory(0), last(0), it(0)
    {
      if (not s->success(s->open(&directory, name))) {
        directory = 0;
      }
    }

    virtual const char* next(unsigned* size) {
      if (it) {
        const char* v = it->next(size);
        if (v) {
          return v;
        } else {
          it->dispose();
          it = 0;
        }
      }

      if (last) {
        s->free(last);
      }

      if (directory) {
        for (const char* v = directory->next(); v; v = directory->next()) {
          if (v[0] != '.') {
            last = append(s, name, "/", v);
            if (s->identify(last) == System::TypeDirectory) {
              it = new (allocate(s, sizeof(Iterator))) Iterator(s, last, skip);
              it->name = last;
            }
            const char* result = last + skip;
            *size = strlen(result);
            return result;
          }
        }
      }

      return 0;
    }

    virtual void dispose() {
      directory->dispose();
      s->free(this);
    }

    System* s;
    const char* name;
    unsigned skip;
    System::Directory* directory;
    const char* last;
    Iterator* it;
  };

  DirectoryElement(System* s, const char* name):
    s(s), name(name)
  { }

  virtual Element::Iterator* iterator() {
    return new (allocate(s, sizeof(Iterator)))
      Iterator(s, name, strlen(name) + 1);
  }

  virtual System::Region* find(const char* name) {
    const char* file = append(s, this->name, "/", name);
    System::Region* region;
    System::Status status = s->map(&region, file);
    s->free(file);

    if (s->success(status)) {
      if (DebugFind) {
        fprintf(stderr, "found %s in %s\n", name, this->name);
      }
      return region;
    } else {
      if (DebugFind) {
        fprintf(stderr, "%s not found in %s\n", name, this->name);
      }
      return 0;
    }
  }

  virtual bool exists(const char* name)  {
    const char* file = append(s, this->name, "/", name);
    System::FileType type = s->identify(file);
    s->free(file);
    return type != System::TypeDoesNotExist;
  }

  virtual void dispose() {
    s->free(name);
    s->free(this);
  }

  System* s;
  const char* name;
};

class PointerRegion: public System::Region {
 public:
  PointerRegion(System* s, const uint8_t* start, size_t length):
    s(s),
    start_(start),
    length_(length)
  { }

  virtual const uint8_t* start() {
    return start_;
  }

  virtual size_t length() {
    return length_;
  }

  virtual void dispose() {
    s->free(this);
  }

  System* s;
  const uint8_t* start_;
  size_t length_;
};

class DataRegion: public System::Region {
 public:
  DataRegion(System* s, size_t length):
    s(s),
    length_(length)
  { }

  virtual const uint8_t* start() {
    return data;
  }

  virtual size_t length() {
    return length_;
  }

  virtual void dispose() {
    s->free(this);
  }

  System* s;
  size_t length_;
  uint8_t data[0];
};

class JarIndex {
 public:
  static const unsigned LocalHeaderSize = 30;
  static const unsigned HeaderSize = 46;

  enum CompressionMethod {
    Stored = 0,
    Deflated = 8
  };

  class Node {
   public:
    Node(uint32_t hash, const uint8_t* entry, Node* next):
      hash(hash), entry(entry), next(next)
    { }

    uint32_t hash;
    const uint8_t* entry;
    Node* next;
  };

  JarIndex(System* s, unsigned capacity):
    s(s),
    capacity(capacity),
    position(0),
    nodes(static_cast<Node*>(allocate(s, sizeof(Node) * capacity)))
  {
    memset(table, 0, sizeof(Node*) * capacity);
  }

  static uint16_t get2(const uint8_t* p) {
    return
      (static_cast<uint16_t>(p[1]) <<  8) |
      (static_cast<uint16_t>(p[0])      );
  }

  static uint32_t get4(const uint8_t* p) {
    return
      (static_cast<uint32_t>(p[3]) << 24) |
      (static_cast<uint32_t>(p[2]) << 16) |
      (static_cast<uint32_t>(p[1]) <<  8) |
      (static_cast<uint32_t>(p[0])      );
  }

  static uint32_t signature(const uint8_t* p) {
    return get4(p);
  }

  static uint16_t compressionMethod(const uint8_t* centralHeader) {
    return get2(centralHeader + 10);
  }

  static uint32_t compressedSize(const uint8_t* centralHeader) {
    return get4(centralHeader + 20);
  }

  static uint32_t uncompressedSize(const uint8_t* centralHeader) {
    return get4(centralHeader + 24);
  }

  static uint16_t fileNameLength(const uint8_t* centralHeader) {
    return get2(centralHeader + 28);
  }

  static uint16_t extraFieldLength(const uint8_t* centralHeader) {
    return get2(centralHeader + 30);
  }

  static uint16_t commentFieldLength(const uint8_t* centralHeader) {
    return get2(centralHeader + 32);
  }

  static uint32_t localHeaderOffset(const uint8_t* centralHeader) {
    return get4(centralHeader + 42);
  }

  static uint16_t localFileNameLength(const uint8_t* localHeader) {
    return get2(localHeader + 26);
  }

  static uint16_t localExtraFieldLength(const uint8_t* localHeader) {
    return get2(localHeader + 28);
  }

  static uint32_t centralDirectoryOffset(const uint8_t* centralHeader) {
    return get4(centralHeader + 16);
  }

  static const uint8_t* fileName(const uint8_t* centralHeader) {
    return centralHeader + 46;
  }

  static const uint8_t* fileData(const uint8_t* localHeader) {
    return localHeader + LocalHeaderSize + localFileNameLength(localHeader) +
      localExtraFieldLength(localHeader);
  }

  static const uint8_t* endOfEntry(const uint8_t* p) {
    return p + HeaderSize + fileNameLength(p) + extraFieldLength(p) +
      commentFieldLength(p);
  }

  static JarIndex* make(System* s, unsigned capacity) {
    return new
      (allocate(s, sizeof(JarIndex) + (sizeof(Node*) * capacity)))
      JarIndex(s, capacity);
  }
  
  static JarIndex* open(System* s, System::Region* region) {
    JarIndex* index = make(s, 32);

    const uint8_t* start = region->start();
    const uint8_t* end = start + region->length();
    const uint8_t* p = end - 22;
    // Find end of central directory record
    while (p > start) {
      if (signature(p) == 0x06054b50) {
	p = region->start() + centralDirectoryOffset(p);
	
	while (p < end) {
	  if (signature(p) == 0x02014b50) {
	    index = index->add(hash(fileName(p), fileNameLength(p)), p);

	    p = endOfEntry(p);
	  } else {
	    return index;
	  }
	}
      } else {
	p--;
      }
    }

    return index;
  }

  JarIndex* add(uint32_t hash, const uint8_t* entry) {
    if (position < capacity) {
      unsigned i = hash & (capacity - 1);
      table[i] = new (nodes + (position++)) Node(hash, entry, table[i]);
      return this;
    } else {
      JarIndex* index = make(s, capacity * 2);
      for (unsigned i = 0; i < capacity; ++i) {
        index->add(nodes[i].hash, nodes[i].entry);
      }
      index->add(hash, entry);
      dispose();
      return index;
    }
  }

  Node* findNode(const char* name) {
    unsigned length = strlen(name);
    unsigned i = hash(name) & (capacity - 1);
    for (Node* n = table[i]; n; n = n->next) {
      const uint8_t* p = n->entry;
      if (equal(name, length, fileName(p), fileNameLength(p))) {
        return n;
      }
    }
    return 0;
  }

  System::Region* find(const char* name, const uint8_t* start) {
    Node* n = findNode(name);
    if (n) {
      const uint8_t* p = n->entry;
      switch (compressionMethod(p)) {
      case Stored: {
        return new (allocate(s, sizeof(PointerRegion)))
          PointerRegion(s, fileData(start + localHeaderOffset(p)),
			compressedSize(p));
      } break;

      case Deflated: {
        DataRegion* region = new
          (allocate(s, sizeof(DataRegion) + uncompressedSize(p)))
          DataRegion(s, uncompressedSize(p));
          
        z_stream zStream; memset(&zStream, 0, sizeof(z_stream));

        zStream.next_in = const_cast<uint8_t*>(fileData(start +
							localHeaderOffset(p)));
        zStream.avail_in = compressedSize(p);
        zStream.next_out = region->data;
        zStream.avail_out = region->length();

        // -15 means max window size and raw deflate (no zlib wrapper)
        int r = inflateInit2(&zStream, -15);
        expect(s, r == Z_OK);

        r = inflate(&zStream, Z_FINISH);
        expect(s, r == Z_STREAM_END);

        inflateEnd(&zStream);

        return region;
      } break;

      default:
        abort(s);
      }
    }

    return 0;
  }

  bool exists(const char* name) {
    return findNode(name) != 0;
  }

  void dispose() {
    s->free(nodes);
    s->free(this);
  }

  System* s;
  unsigned capacity;
  unsigned position;
  
  Node* nodes;
  Node* table[0];
};

class JarElement: public Element {
 public:
  class Iterator: public Element::Iterator {
   public:
    Iterator(System* s, JarIndex* index): s(s), index(index), position(0) { }

    virtual const char* next(unsigned* size) {
      if (position < index->position) {
        JarIndex::Node* n = index->nodes + (position++);
        *size = JarIndex::fileNameLength(n->entry);
        return reinterpret_cast<const char*>(JarIndex::fileName(n->entry));
      } else {
        return 0;
      }
    }

    virtual void dispose() {
      s->free(this);
    }

    System* s;
    JarIndex* index;
    unsigned position;
  };

  JarElement(System* s, const char* name):
    s(s), name(name), region(0), index(0)
  { }

  virtual Element::Iterator* iterator() {
    init();

    return new (allocate(s, sizeof(Iterator))) Iterator(s, index);
  }

  virtual void init() {
    if (index == 0) {
      System::Region* r;
      if (s->success(s->map(&r, name))) {
        region = r;
        index = JarIndex::open(s, r);
      }
    }
  }

  virtual System::Region* find(const char* name) {
    init();

    while (*name == '/') name++;

    System::Region* r = (index ? index->find(name, region->start()) : 0);
    if (DebugFind) {
      if (r) {
        fprintf(stderr, "found %s in %s\n", name, this->name);
      } else {
        fprintf(stderr, "%s not found in %s\n", name, this->name);
      }
    }
    return r;
  }

  virtual bool exists(const char* name)  {
    init();

    while (*name == '/') name++;

    return (index ? index->exists(name) : 0);
  }

  virtual void dispose() {
    s->free(name);
    if (index) {
      index->dispose();
    }
    if (region) {
      region->dispose();
    }
    s->free(this);
  }

  System* s;
  const char* name;
  System::Region* region;
  JarIndex* index;
};

class BuiltinElement: public JarElement {
 public:
  BuiltinElement(System* s, const char* name, const char* libraryName):
    JarElement(s, name),
    libraryName(libraryName ? copy(s, libraryName) : 0)
  { }

  virtual void init() {
    if (index == 0) {
      if (s->success(s->load(&library, libraryName))) {
        void* p = library->resolve(name);
        if (p) {
          uint8_t* (*function)(unsigned*);
          memcpy(&function, &p, BytesPerWord);

          unsigned size;
          uint8_t* data = function(&size);
          if (data) {
            region = new (allocate(s, sizeof(PointerRegion)))
              PointerRegion(s, data, size);
            index = JarIndex::open(s, region);
          }
        }
      }
    }
  }

  virtual void dispose() {
    library->disposeAll();
    s->free(libraryName);
    JarElement::dispose();
  }

  System::Library* library;
  const char* libraryName;
};

Element*
parsePath(System* s, const char* path, const char* bootLibrary)
{
  Element* first = 0;
  Element* prev = 0;
  for (Tokenizer t(path, s->pathSeparator()); t.hasMore();) {
    Tokenizer::Token token(t.next());

    Element* e;
    if (*token.s == '[' and token.s[token.length - 1] == ']') {
      char* name = static_cast<char*>(allocate(s, token.length - 1));
      memcpy(name, token.s + 1, token.length - 1);
      name[token.length - 2] = 0; 
  
      e = new (allocate(s, sizeof(BuiltinElement)))
        BuiltinElement(s, name, bootLibrary);
    } else {
      char* name = static_cast<char*>(allocate(s, token.length + 1));
      memcpy(name, token.s, token.length);
      name[token.length] = 0;

      switch (s->identify(name)) {
      case System::TypeFile: {
        e = new (allocate(s, sizeof(JarElement))) JarElement(s, name);
      } break;

      case System::TypeDirectory: {
        e = new (allocate(s, sizeof(DirectoryElement)))
          DirectoryElement(s, name);
      } break;

      default: {
        s->free(name);
        e = 0;
      } break;
      }
    }

    if (e) {
      if (prev) {
        prev->next = e;
      } else {
        first = e;
      }
      prev = e;
    }
  }

  return first;
}

class MyIterator: public Finder::IteratorImp {
 public:
  MyIterator(System* s, Element* path):
    s(s), e(path ? path->next : 0), it(path ? path->iterator() : 0)
  { }

  virtual const char* next(unsigned* size) {
    while (it) {
      const char* v = it->next(size);
      if (v) {
        return v;
      } else {
        it->dispose();
        if (e) {
          it = e->iterator();
          e = e->next;
        } else {
          it = 0;
        }
      }
    }
    return 0;
  }

  virtual void dispose() {
    if (it) it->dispose();
    s->free(this);
  }

  System* s;
  Element* e;
  Element::Iterator* it;
};

class MyFinder: public Finder {
 public:
  MyFinder(System* system, const char* path, const char* bootLibrary):
    system(system),
    path_(parsePath(system, path, bootLibrary)),
    pathString(copy(system, path))
  { }

  virtual IteratorImp* iterator() {
    return new (allocate(system, sizeof(MyIterator)))
      MyIterator(system, path_);
  }

  virtual System::Region* find(const char* name) {
    for (Element* e = path_; e; e = e->next) {
      System::Region* r = e->find(name);
      if (r) {
        return r;
      }
    }
    
    return 0;
  }

  virtual bool exists(const char* name) {
    for (Element* e = path_; e; e = e->next) {
      if (e->exists(name)) {
        return true;
      }
    }
    
    return false;
  }

  virtual const char* path() {
    return pathString;
  }

  virtual void dispose() {
    for (Element* e = path_; e;) {
      Element* t = e;
      e = e->next;
      t->dispose();
    }
    system->free(pathString);
    system->free(this);
  }

  System* system;
  Element* path_;
  const char* pathString;
};

} // namespace

namespace vm {

Finder*
makeFinder(System* s, const char* path, const char* bootLibrary)
{
  return new (allocate(s, sizeof(MyFinder))) MyFinder(s, path, bootLibrary);
}

} // namespace vm
