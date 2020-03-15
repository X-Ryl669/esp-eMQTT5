#ifndef hpp_CPP_MQTT_CPP_hpp
#define hpp_CPP_MQTT_CPP_hpp

// We need basic types
#include "../../Types.hpp"
// We need Platform code for allocations too
#include "../../Platform/Platform.hpp"

#if (MQTTDumpCommunication == 1)
  // Because all projects are different, it's hard to give a generic method for dumping elements.
  // So we end up with only limited dependencies:  
  // - a string class that concatenate with operator +=
  // - the string class has MQTTStringGetData(MQTTString instance) method returning a pointer to the string array 
  // - the string class has MQTTStringGetLength(MQTTString instance) method returning the array size of the string 
  // - a hexadecimal dumping method
  // - a printf-like formatting function

  // Feel free to define the string class and method to use beforehand if you want to use your own.
  // If none provided, we use's std::string class (or ClassPath's FastString depending on the environment)
  #ifndef MQTTStringPrintf
    static MQTTString MQTTStringPrintf(const char * format, ...)
    {
        va_list argp;
        va_start(argp, format);
        char buf[512];
        // We use vasprintf extension to avoid dual parsing of the format string to find out the required length
        int err = vsnprintf(buf, sizeof(buf), format, argp);
        va_end(argp);
        if (err <= 0) return MQTTString();
        if (err >= (int)sizeof(buf)) err = (int)(sizeof(buf) - 1);
        buf[err] = 0;
        return MQTTString(buf, (size_t)err);
    }
  #endif
  #ifndef MQTTHexDump 
    static void MQTTHexDump(MQTTString & out, const uint8* bytes, const uint32 length)
    {
        for (uint32 i = 0; i < length; i++) 
            out += MQTTStringPrintf("%02X", bytes[i]);
    }
  #endif
#endif

/** All network protocol specific structure or enumerations are declared here */
namespace Protocol
{
    /** The MQTT specific enumeration or structures */
    namespace MQTT
    {
        /** The types declared in this namespace are shared between the different versions */
        namespace Common
        {
            /** This is the standard error code while reading an invalid value from hostile source */
            enum LocalError
            {
                BadData         = 0xFFFFFFFF,   //!< Malformed data
                NotEnoughData   = 0xFFFFFFFE,   //!< Not enough data
                Shortcut        = 0xFFFFFFFD,   //!< Serialization shortcut used (not necessarly an error)
                
                MinErrorCode    = 0xFFFFFFFD,
            };

            /** Quickly check if the given code is an error */
            static inline bool isError(uint32 value) { return value >= MinErrorCode; }
            /** Check if serialization shortcut was used */
            static inline bool isShortcut(uint32 value) { return value == Shortcut; }


            /** The base interface all MQTT serializable structure must implement */
            struct Serializable
            {
                /** We have a getSize() method that gives the number of bytes requires to serialize this object */
                virtual uint32 getSize() const = 0;
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least getSize() bytes long
                    @return The number of bytes used in the buffer */
                virtual uint32 copyInto(uint8 * buffer) const = 0;
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or a LocalError upon error (use isError() to test for it) */
                virtual uint32 readFrom(const uint8 * buffer, uint32 bufLength) = 0;

#if MQTTDumpCommunication == 1
                /** Dump the serializable to the given string */
                virtual void dump(MQTTString & out, const int indent = 0) = 0;
#endif 

                /** Check if this object is correct after deserialization */
                virtual bool check() const { return true; }
                /** Required destructor */
                virtual ~Serializable() {}
            };
            
            /** Empty serializable used for generic code to avoid useless specific case in packet serialization */
            struct EmptySerializable : public Serializable
            {
                uint32 getSize() const { return 0; }
                uint32 copyInto(uint8 *) const { return 0; }
                uint32 readFrom(const uint8 *, uint32) { return 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*s%s\n", indent, "", "<none>"); }
#endif
                bool check() const { return true; }
            };
            
            /** Invalid serialization used as an escape path */
            struct InvalidData : public Serializable
            {
                uint32 getSize() const { return 0; }
                uint32 copyInto(uint8 *) const { return 0; }
                uint32 readFrom(const uint8 *, uint32) { return BadData; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*s%s\n", indent, "", "<invalid>"); }
#endif
                bool check() const { return false; }
            };
            
            /** A serializable with suicide characteristics 
                Typically, some structure are using list internally. 
                You can build list either by chaining items that are allocated with new or stack allocated.
                In that case, you can't delete the next pointer inconditionnally, you need to got through a specialization
                depending on the allocation type (heap or stack).
                So instead call suicide and the stack based topic won't delete upon suiciding */
            struct SerializableWithSuicide : public Serializable
            {
                /** Commit suicide. This is overloaded on stack allocated properties to avoid calling delete here */
                virtual void suicide() { delete this; }
            };

            /** The serialization of memory mapped structures.
                They must have a fromNetwork / toNetwork function.
                It's using CRTP here to inject Serializable-like interface to a basic memory-mapped structure.
                We don't use any virtual table here, so when the compiler is instructed to pack-fit the structure,
                it (does/should) remove any virtual table here, thus keeping the struct size as if this wasn't
                deriving from this struct.
             
                This is called empty base optimization and it's required for StandardLayoutType in C++ standard,
                so we make use of this here. */
            template <typename T>
            struct MemoryMapped
            {
                uint32 getSize() const { return sizeof(T); }
                uint32 copyInto(uint8 * buffer) const
                {
                    const_cast<T*>(static_cast<const T*>(this))->toNetwork();
                    memcpy(buffer, static_cast<const T*>(this), sizeof(T));
                    const_cast<T*>(static_cast<const T*>(this))->fromNetwork();
                    
                    return sizeof(T);
                }
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < sizeof(T)) return NotEnoughData;
                    memcpy(static_cast<T*>(this), buffer, sizeof(T));
                    static_cast<T*>(this)->fromNetwork();
                    return sizeof(T);
                }
            };
            

            /** The visitor that'll be called with the relevant value */
            struct MemMappedVisitor
            {
                /** Accept the given buffer */
                virtual uint32 acceptBuffer(const uint8 * buffer, const uint32 bufLength) = 0; 
                // All visitor will have a getValue() method, but the returned type depends on the visitor and thus,
                // can not be declared polymorphically
#if MQTTDumpCommunication == 1
                virtual void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*s", (int)indent, "");
                    // Voluntary incomplete
                }
#endif                


                /** Default destructor */
                virtual ~MemMappedVisitor() {}
            };

            /** Plumbing code for simple visitor pattern to avoid repetitive code in this file */
            template <typename T>
            struct SerializableVisitor : public MemMappedVisitor 
            {
                T & getValue() { return *static_cast<T*>(this); }
                operator T& () { return getValue(); } 
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    static_cast<T*>(this)->dump(out, indent);
                }
#endif

                uint32 acceptBuffer(const uint8 * buffer, const uint32 bufLength) { return static_cast<T*>(this)->readFrom(buffer, bufLength); }
            };

            /** Plumbing code for simple visitor pattern to avoid repetitive code in this file */
            template <typename T>
            struct PODVisitor : public MemMappedVisitor
            {
                T value;
                T & getValue() { return value; }

                operator T& () { return getValue(); } 
                PODVisitor(const T value = 0) : value(value) {}

                uint32 acceptBuffer(const uint8 * buffer, const uint32 bufLength) 
                { 
                    if (bufLength < sizeof(value)) return NotEnoughData;
                    memcpy(&value, buffer, sizeof(value)); 
                    return sizeof(value); 
                }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    MemMappedVisitor::dump(out, indent);
                    out += getValue();
                    out += "\n";
                }
#endif

            };
        
            /** Plumbing code for simple visitor pattern to avoid repetitive code in this file */
            template <typename T>
            struct LittleEndianPODVisitor : public MemMappedVisitor
            {
                T value;
                T & getValue() { return value; }

                operator T& () { return getValue(); } 
                LittleEndianPODVisitor(const T value = 0) : value(value) {}

                uint32 acceptBuffer(const uint8 * buffer, const uint32 bufLength) 
                { 
                    if (bufLength < sizeof(value)) return NotEnoughData;
                    memcpy(&value, buffer, sizeof(value)); 
                    value = BigEndian(value);
                    return sizeof(value); 
                }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    MemMappedVisitor::dump(out, indent);
                    out += getValue();
                    out += "\n";
                }
#endif

            };


#pragma pack(push, 1)
            /** A MQTT string is basically a string with a BigEndian size going first (section 1.5.4) */
            struct String
            {
                /** The string length in bytes */
                uint16 length;
                char   data[];
                
                /** Call this method to read the structure when it's casted from the network buffer */
                void fromNetwork() { length = ntohs(length); }
                /** Call this method before sending the structure to the network */
                void toNetwork()   { length = htons(length); }
            };
            
            /** A string that's memory managed itself */
            struct DynamicString : public Serializable
            {
                /** The string length in bytes */
                uint16      length;
                /** The data itself */
                char   *    data;

                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return (uint32)length + 2; }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 4 bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { uint16 size = BigEndian(length); memcpy(buffer, &size, 2); memcpy(buffer+2, data, length); return (uint32)length + 2; }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer that's at least 4 bytes long
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < 2) return NotEnoughData;
                    uint16 size = 0; memcpy(&size, buffer, 2); length = ntohs(size);
                    if (length+2 > bufLength) return NotEnoughData;
                    data = (char*)Platform::safeRealloc(data, length);
                    memcpy(data, buffer+2, length);
                    return (uint32)length+2;
                }
                /** Check if the value is correct */
                bool check() const { return data ? length : length == 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sStr (%d bytes): %.*s\n", (int)indent, "", (int)length, length, data); }
#endif
                
                /** Default constructor */
                DynamicString() : length(0), data(0) {}
                /** Construct from a text */
                DynamicString(const char * text) : length(text ? strlen(text) : 0), data((char*)Platform::malloc(length)) { memcpy(data, text, length); }
#if (MQTTDumpCommunication == 1)
                /** Construct from a FastString */
                DynamicString(const MQTTString & text) : length(MQTTStringGetLength(text)), data((char*)Platform::malloc(length)) { memcpy(data, MQTTStringGetData(text), length); }
                /** Construct from a FastString */
                DynamicString(const MQTTROString & text) : length(MQTTStringGetLength(text)), data((char*)Platform::malloc(length)) { memcpy(data, MQTTStringGetData(text), length); }
#endif
                /** Copy constructor */
                DynamicString(const DynamicString & other) : length(other.length), data((char*)Platform::malloc(length)) { memcpy(data, other.data, length); }
#if HasCPlusPlus11 == 1
                /** Move constructor */
                DynamicString(DynamicString && other) : length(std::move(other.length)), data(std::move(other.data)) { }
#endif
                /** Destructor */
                ~DynamicString() { Platform::free(data); length = 0; }

#if (MQTTDumpCommunication == 1)
                /** Convert to a ReadOnlyString */
                operator MQTTROString() const { return MQTTROString(data, length); }
                /** Comparison operator */
                bool operator != (const MQTTROString & other) const { return length != MQTTStringGetLength(other) || memcmp(data, MQTTStringGetData(other), length); }
                /** Comparison operator */
                bool operator == (const MQTTROString & other) const { return length == MQTTStringGetLength(other) && memcmp(data, MQTTStringGetData(other), length) == 0; }
#endif                
                /** Copy operator */
                DynamicString & operator = (const DynamicString & other) { if (this != &other) { this->~DynamicString(); length = other.length; data = (char*)Platform::malloc(length); memcpy(data, other.data, length); } return *this; }
                /** Copy operator */
                void from(const char * str, const size_t len = 0) { this->~DynamicString(); length = len ? len : (strlen(str)+1); data = (char*)Platform::malloc(length); memcpy(data, str, length); data[length - 1] = 0; }

            };
            
            /** A dynamic string pair */
            struct DynamicStringPair : public Serializable
            {
                /** The key used for the pair */
                DynamicString key;
                /** The value used for the pair */
                DynamicString value;
                
                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return key.getSize() + value.getSize(); }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least getSize() bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { uint32 o = key.copyInto(buffer); o += value.copyInto(buffer+o); return o; }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer that's at least 4 bytes long
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    uint32 o = key.readFrom(buffer, bufLength);
                    if (isError(o)) return o;
                    uint32 s = value.readFrom(buffer + o, bufLength - o);
                    if (isError(s)) return s;
                    return s+o;
                }
                /** Check if the value is correct */
                bool check() const { return key.check() && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sKV:\n", (int)indent, ""); key.dump(out, indent + 2); value.dump(out, indent + 2); }
#endif

                /** Default constructor */
                DynamicStringPair(const DynamicString & k = "", const DynamicString & v = "") : key(k), value(v) {}
                /** Copy constructor */
                DynamicStringPair(const DynamicStringPair & other) : key(other.key), value(other.value) {}
#if HasCPlusPlus11 == 1
                /** Move constructor */
                DynamicStringPair(DynamicStringPair && other) : key(std::move(other.key)), value(std::move(other.value)) { }
#endif
            };

            /** A MQTT binary data with a BigEndian size going first (section 1.5.6) */
            struct BinaryData
            {
                /** The data length in bytes */
                uint16 length;
                uint8  data[];
                
                /** Call this method to read the structure when it's casted from the network buffer */
                void fromNetwork() { length = ntohs(length); }
                /** Call this method before sending the structure to the network */
                void toNetwork()   { length = htons(length); }
            };
            
            /** A dynamic binary data, with self managed memory */
            struct DynamicBinaryData : public Serializable
            {
                /** The string length in bytes */
                uint16      length;
                /** The data itself */
                uint8   *    data;

                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return (uint32)length + 2; }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 4 bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { uint16 size = htons(length); memcpy(buffer, &size, 2); memcpy(buffer+2, data, length); return (uint32)length + 2; }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer that's at least 4 bytes long
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < 2) return NotEnoughData;
                    uint16 size = 0; memcpy(&size, buffer, 2); length = ntohs(size);
                    if (length+2 > bufLength) return NotEnoughData;
                    data = (uint8*)Platform::safeRealloc(data, length);
                    memcpy(data, buffer+2, length);
                    return (uint32)length + 2;
                }
                /** Check if the value is correct */
                bool check() const { return data ? length : length == 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sBin (%d bytes):", (int)indent, "", (int)length); MQTTHexDump(out, data, length); out += "\n"; }
#endif
                
                /** Construct from a memory block */
                DynamicBinaryData(const uint16 length = 0, const uint8 * block = 0) : length(length), data(length ? (uint8*)Platform::malloc(length) : (uint8*)0) { memcpy(data, block, length); }
                /** Copy constructor */
                DynamicBinaryData(const DynamicBinaryData & other) : length(other.length), data(length ? (uint8*)Platform::malloc(length) : (uint8*)0) { memcpy(data, other.data, length); }
#if HasCPlusPlus11 == 1
                /** Move constructor */
                DynamicBinaryData(DynamicBinaryData && other) : length(std::move(other.length)), data(std::move(other.data)) { }
#endif
                /** Destructor */
                ~DynamicBinaryData() { Platform::free(data); length = 0; }
            };



            /** A read only dynamic string view. 
                This is used to avoid copying a string buffer when only a pointer is required. 
                This string can be mutated to many buffer but no modification is done to the underlying array of chars */
            struct DynamicStringView : public Serializable, public SerializableVisitor<DynamicStringView>
            {
                /** The string length in bytes */
                uint16          length;
                /** The data itself */
                const char *    data;

                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return (uint32)length + 2; }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 4 bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { uint16 size = htons(length); memcpy(buffer, &size, 2); memcpy(buffer+2, data, length); return (uint32)length + 2; }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer that's at least 4 bytes long
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or BadData upon error
                    @warning This method capture a pointer on the given buffer so it must outlive this object when being called.
                             Don't use this method if buffer is a temporary data. */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {   
                    if (bufLength < 2) return NotEnoughData;
                    uint16 size = 0; memcpy(&size, buffer, 2); length = ntohs(size);
                    if (length+2 > bufLength) return NotEnoughData;
                    data = (const char*)&buffer[2];
                    return (uint32)length + 2;
                }


                /** Check if the value is correct */
                bool check() const { return data ? length : length == 0; }

#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sStr (%d bytes): %.*s\n", (int)indent, "", (int)length, length, data); }
#endif


                /** Capture from a dynamic string here.
                    Beware of this method as the source must outlive this instance */
                DynamicStringView & operator = (const DynamicString & source) { length = source.length; data = source.data; return *this; }
                /** Capture from a dynamic string here.
                    Beware of this method as the source must outlive this instance */
                DynamicStringView & operator = (const char * string) { length = strlen(string); data = string; return *this; }
                /** Basic operator */
                DynamicStringView & operator = (const DynamicStringView & source) { length = source.length; data = source.data; return *this; }

                /** Comparison operator */
                bool operator != (const DynamicStringView & other) const { return length != other.length || memcmp(data, other.data, length); }
                /** Comparison operator */
                bool operator == (const DynamicStringView & other) const { return length == other.length && memcmp(data, other.data, length) == 0; }
                /** Comparison operator */
                bool operator == (const char * other) const { return length == strlen(other) && memcmp(data, other, length) == 0; }

                /** From a usual dynamic string */
                DynamicStringView(const DynamicString & other) : length(other.length), data(other.data) {}
                /** From a given C style buffer */
                DynamicStringView(const char * string) : length(strlen(string)), data(string) {}
                /** A null version */
                DynamicStringView() : length(0), data(0) {}

            };

            /** A dynamic string pair view.
                This is used to avoid copying a string buffer when only a pointer is required. 
                This string can be mutated to many buffer but no modification is done to the underlying array of chars */
            struct DynamicStringPairView : public Serializable, public SerializableVisitor<DynamicStringPairView>
            {
                /** The key used for the pair */
                DynamicStringView key;
                /** The value used for the pair */
                DynamicStringView value;
                
                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return key.getSize() + value.getSize(); }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least getSize() bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { uint32 o = key.copyInto(buffer); o += value.copyInto(buffer+o); return o; }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer that's at least 4 bytes long
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    uint32 o = key.readFrom(buffer, bufLength);
                    if (isError(o)) return o;
                    uint32 s = value.readFrom(buffer + o, bufLength - o);
                    if (isError(s)) return s;
                    return s+o;
                }
                /** Check if the value is correct */
                bool check() const { return key.check() && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sKV:\n", (int)indent, ""); key.dump(out, indent + 2); value.dump(out, indent + 2); }
#endif

                /** Default constructor */
                DynamicStringPairView(const DynamicStringView & k = "", const DynamicStringView & v = "") : key(k), value(v) {}
                /** Copy constructor */
                DynamicStringPairView(const DynamicStringPairView & other) : key(other.key), value(other.value) {}
#if HasCPlusPlus11 == 1
                /** Move constructor */
                DynamicStringPairView(DynamicStringPair && other) : key(std::move(other.key)), value(std::move(other.value)) { }
#endif
            };
            
            /** A read only dynamic dynamic binary data, without self managed memory. 
                This is used to avoid copying a binary data buffer when only a pointer is required. */
            struct DynamicBinDataView : public Serializable, public SerializableVisitor<DynamicBinDataView>
            {
                /** The string length in bytes */
                uint16             length;
                /** The data itself */
                const uint8   *    data;

                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return (uint32)length + 2; }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 4 bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { uint16 size = htons(length); memcpy(buffer, &size, 2); memcpy(buffer+2, data, length); return (uint32)length + 2; }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer that's at least 4 bytes long
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or BadData upon error
                    @warning This method capture a pointer on the given buffer so it must outlive this object when being called.
                             Don't use this method if buffer is a temporary data. */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {   
                    if (bufLength < 2) return NotEnoughData;
                    uint16 size = 0; memcpy(&size, buffer, 2); length = ntohs(size);
                    if (length+2 > bufLength) return NotEnoughData;
                    data = &buffer[2];
                    return (uint32)length + 2;
                }
                /** Check if the value is correct */
                bool check() const { return data ? length : length == 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sBin (%d bytes):", (int)indent, "", (int)length); MQTTHexDump(out, data, length); out += "\n"; }
#endif
                
                /** Construct from a memory block */
                DynamicBinDataView(const uint16 length = 0, const uint8 * block = 0) : length(length), data(block) { }
                /** Copy constructor */
                DynamicBinDataView(const DynamicBinaryData & other) : length(other.length), data(other.data) { }

            
                /** Capture from a dynamic string here.
                    Beware of this method as the source must outlive this instance */
                DynamicBinDataView & operator = (const DynamicBinaryData & source) { length = source.length; data = source.data; return *this; }
                /** Basic operator */
                DynamicBinDataView & operator = (const DynamicBinDataView & source) { length = source.length; data = source.data; return *this; }

            };

#pragma pack(pop)

            /** The variable byte integer encoding (section 1.5.5).
                It's always stored encoded as a network version */
            struct VBInt : public Serializable
            {
                enum 
                { 
                    MaxSizeOn1Byte  = 127,
                    MaxSizeOn2Bytes = 16383,
                    MaxSizeOn3Bytes = 2097151, 
                    MaxPossibleSize = 268435455, //!< The maximum possible size
                };

                union
                {
                    /** In the worst case, it's using 32 bits */
                    uint8   value[4];
                    /** The quick accessible word */
                    uint32  word;
                };
                /** The actual used size for transmitting the value, in bytes */
                uint16  size;
                
                /** Set the value. This algorithm is 26% faster compared to the basic method shown in the standard */
                VBInt & operator = (uint32 other)
                {
                    uint8 carry = 0;
                    uint8 pseudoLog = (other > 127) + (other > 16383) + (other > 2097151) + (other > 268435455);
                    size = pseudoLog+1;
                    switch (pseudoLog)
                    {
                    case 3: value[pseudoLog--] = (other >> 21); other &= 0x1FFFFF; carry = 0x80; // Intentionally no break here
                    // fall through
                    case 2: value[pseudoLog--] = (other >> 14) | carry; other &= 0x3FFF; carry = 0x80; // Same
                    // fall through
                    case 1: value[pseudoLog--] = (other >>  7) | carry; other &= 0x7F; carry = 0x80; // Ditto
                    // fall through
                    case 0: value[pseudoLog--] = other | carry; return *this;
                    default:
                    case 4: value[0] = value[1] = value[2] = value[3] = 0xFF; size = 0; return *this;  // This is an error anyway
                    }
                }
                /** Get the value as an unsigned integer (decode)
                    @warning No check is made here to assert the encoding is good. Use check() to assert the encoding. */
                operator uint32 () const
                {
                    uint32 o = 0;
                    switch(size)
                    {
                    case 0: return 0; // This is an error anyway
                    case 4: o = value[3] << 21;            // Intentionally no break here
                    // fall through
                    case 3: o |= (value[2] & 0x7F) << 14;  // Same
                    // fall through
                    case 2: o |= (value[1] & 0x7F) << 7;   // Ditto
                    // fall through
                    case 1: o |= value[0] & 0x7F; // Break is useless here too
                    }
                    return o;
                }
                
                /** Check if the value is correct */
                bool check() const
                {
                    return size > 0 && size < 5 && (value[size-1] & 0x80) == 0;
                }
                /** For consistancy with the other structures, we have a getSize() method that gives the number of bytes requires to serialize this object */
                uint32 getSize() const { return size; }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 4 bytes long
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { memcpy(buffer, value, size); return size; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 4 bytes long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    for (size = 0; size < 4;)
                    {
                        if (size+1 > bufLength) return NotEnoughData;
                        value[size] = buffer[size];
                        if (value[size++] < 0x80) break;
                    }
                    return size < 4 ? size : (uint32)BadData;
                }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sVBInt: %u\n", (int)indent, "", (uint32)*this); }
#endif

                /** Default constructor */
                VBInt(uint32 value = 0) { this->operator=(value); }
                /** Copy constructor */
                VBInt(const VBInt & other) : word(other.word), size(other.size) { }
            };

            /** The variable byte integer encoding (section 1.5.5).
                It's always stored encoded as a network version */
            struct MappedVBInt : public PODVisitor<uint32>
            {
                /** Get the value from this mapped variable byte integer
                    @param buffer       A pointer to the buffer to read from
                    @param bufLength    The length of the buffer to read from 
                    @return the number of bytes used in the buffer */
                uint32 acceptBuffer(const uint8 * buffer, const uint32 bufLength)
                {  
                    uint32 size = 0; uint32 & o = getValue(); o = 0;
                    for (size = 0; size < 4 && size < bufLength; )
                    {
                        if (size+1 > bufLength) return NotEnoughData;
                        if (buffer[size++] < 0x80) break;
                    }

                    switch(size)
                    {
                    default:
                    case 0: return BadData; // This is an error anyway
                    case 4: o = buffer[3] << 21;            // Intentionally no break here
                    // fall through
                    case 3: o |= (buffer[2] & 0x7F) << 14;  // Same
                    // fall through
                    case 2: o |= (buffer[1] & 0x7F) << 7;   // Ditto
                    // fall through
                    case 1: o |= buffer[0] & 0x7F; // Break is useless here too
                    }
                    return size;
                }
            };

            


            /** The control packet type.
                Src means the expected direction, C is for client to server, S for server to client and B for both direction. */
            enum ControlPacketType
            {
                RESERVED        = 0,    //!< Src:Forbidden, it's reserved
                CONNECT         = 1,    //!< Src:C Connection requested
                CONNACK         = 2,    //!< Src:S Connection acknowledged
                PUBLISH         = 3,    //!< Src:B Publish message
                PUBACK          = 4,    //!< Src:B Publish acknowledged (QoS 1)
                PUBREC          = 5,    //!< Src:B Publish received (QoS 2 delivery part 1)
                PUBREL          = 6,    //!< Src:B Publish released (QoS 2 delivery part 2)
                PUBCOMP         = 7,    //!< Src:B Publish completed (QoS 2 delivery part 3)
                SUBSCRIBE       = 8,    //!< Src:C Subscribe requested
                SUBACK          = 9,    //!< Src:S Subscribe acknowledged
                UNSUBSCRIBE     = 10,   //!< Src:C Unsubscribe requested
                UNSUBACK        = 11,   //!< Src:S Unsubscribe acknowledged
                PINGREQ         = 12,   //!< Src:C Ping requested
                PINGRESP        = 13,   //!< Src:S Ping answered
                DISCONNECT      = 14,   //!< Src:B Disconnect notification
                AUTH            = 15,   //!< Src:B Authentication exchanged
            };
            
            static const char * getControlPacketName(ControlPacketType type)
            {
                static const char * names[16] = { "RESERVED", "CONNECT", "CONNACK", "PUBLISH", "PUBACK", "PUBREC", "PUBREL", "PUBCOMP", "SUBSCRIBE", "SUBACK",
                                                  "UNSUBSCRIBE", "UNSUBACK", "PINGREQ", "PINGRESP", "DISCONNECT", "AUTH" };
                return names[(int)type];
            }

        }
    
        /** The version 5 for this protocol (OASIS MQTTv5 http://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html ) */
        namespace V5
        {
            // Bring shared types here
            using namespace Protocol::MQTT::Common;
            
            /** The reason codes */
            enum ReasonCodes
            {
                Success                             = 0x00, //!< Success
                NormalDisconnection                 = 0x00, //!< Normal disconnection
                GrantedQoS0                         = 0x00, //!< Granted QoS 0
                GrantedQoS1                         = 0x01, //!< Granted QoS 1
                GrantedQoS2                         = 0x02, //!< Granted QoS 2
                DisconnectWithWillMessage           = 0x04, //!< Disconnect with Will Message
                NoMatchingSubscribers               = 0x10, //!< No matching subscribers
                NoSubscriptionExisted               = 0x11, //!< No subscription existed
                ContinueAuthentication              = 0x18, //!< Continue authentication
                ReAuthenticate                      = 0x19, //!< Re-authenticate
                UnspecifiedError                    = 0x80, //!< Unspecified error
                MalformedPacket                     = 0x81, //!< Malformed Packet
                ProtocolError                       = 0x82, //!< Protocol Error
                ImplementationSpecificError         = 0x83, //!< Implementation specific error
                UnsupportedProtocolVersion          = 0x84, //!< Unsupported Protocol Version
                ClientIdentifierNotValid            = 0x85, //!< Client Identifier not valid
                BadUserNameOrPassword               = 0x86, //!< Bad User Name or Password
                NotAuthorized                       = 0x87, //!< Not authorized
                ServerUnavailable                   = 0x88, //!< Server unavailable
                ServerBusy                          = 0x89, //!< Server busy
                Banned                              = 0x8A, //!< Banned
                ServerShuttingDown                  = 0x8B, //!< Server shutting down
                BadAuthenticationMethod             = 0x8C, //!< Bad authentication method
                KeepAliveTimeout                    = 0x8D, //!< Keep Alive timeout
                SessionTakenOver                    = 0x8E, //!< Session taken over
                TopicFilterInvalid                  = 0x8F, //!< Topic Filter invalid
                TopicNameInvalid                    = 0x90, //!< Topic Name invalid
                PacketIdentifierInUse               = 0x91, //!< Packet Identifier in use
                PacketIdentifierNotFound            = 0x92, //!< Packet Identifier not found
                ReceiveMaximumExceeded              = 0x93, //!< Receive Maximum exceeded
                TopicAliasInvalid                   = 0x94, //!< Topic Alias invalid
                PacketTooLarge                      = 0x95, //!< Packet too large
                MessageRateTooHigh                  = 0x96, //!< Message rate too high
                QuotaExceeded                       = 0x97, //!< Quota exceeded
                AdministrativeAction                = 0x98, //!< Administrative action
                PayloadFormatInvalid                = 0x99, //!< Payload format invalid
                RetainNotSupported                  = 0x9A, //!< Retain not supported
                QoSNotSupported                     = 0x9B, //!< QoS not supported
                UseAnotherServer                    = 0x9C, //!< Use another server
                ServerMoved                         = 0x9D, //!< Server moved
                SharedSubscriptionsNotSupported     = 0x9E, //!< Shared Subscriptions not supported
                ConnectionRateExceeded              = 0x9F, //!< Connection rate exceeded
                MaximumConnectTime                  = 0xA0, //!< Maximum connect time
                SubscriptionIdentifiersNotSupported = 0xA1, //!< Subscription Identifiers not supported
                WildcardSubscriptionsNotSupported   = 0xA2, //!< Wildcard Subscriptions not supported
            };

            /** The dynamic string class we prefer using depends on whether we are using client or server code */
#if MQTTClientOnlyImplementation == 1
            typedef DynamicStringView   DynString;
            typedef DynamicBinDataView  DynBinData;
#else
            typedef DynamicString       DynString;
            typedef DynamicBinaryData   DynBinData;
#endif




#pragma pack(push, 1)
            /** A MQTT fixed header (section 2.1.1).
                This is not used directly, but only to remember the expected format. Instead, each packet type is declared underneath, since it's faster to parse them directly */
            struct FixedHeader
            {
#if IsBigEndian == 1
                union
                {
                    uint8 raw : 8;
                    struct
                    {
                        /** The packet type */
                        uint8 type : 4;
                        uint8 dup : 1;
                        uint8 QoS : 2;
                        uint8 retain : 1;
                    };
                };
#else
                union
                {
                    uint8 raw : 8;
                    struct
                    {
                        uint8 retain : 1;
                        uint8 QoS : 2;
                        uint8 dup : 1;
                        /** The packet type */
                        uint8 type : 4;
                    };
                };
#endif
            };
            
            /** The common format for the fixed header type */
            template <ControlPacketType type, uint8 flags>
            struct FixedHeaderType
            {
                const uint8 typeAndFlags;
                ControlPacketType   getType() const { return type; }
                uint8               getFlags() const { return flags; }
                bool                check() const { return (typeAndFlags & 0xF) == flags; }
                static bool         check(const uint8 flag) { return flag == flags; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sHeader: (type %s, no flags)\n", (int)indent, "", getControlPacketName(type)); }
#endif
                
                FixedHeaderType() : typeAndFlags(((uint8)type << 4) | flags) {}
            };
            
            /** The only header where flags have a meaning is for Publish operation */
            template <>
            struct FixedHeaderType<PUBLISH, 0>
            {
                uint8 typeAndFlags;
                
                ControlPacketType getType() const { return PUBLISH; }
                
                uint8 getFlags() const { return typeAndFlags & 0xF; }
                bool isDup()     const { return typeAndFlags & 0x8; }
                bool isRetain()  const { return typeAndFlags & 0x1; }
                uint8 getQoS()   const { return (typeAndFlags & 0x6) >> 1; }
                
                void setDup(const bool e)       { typeAndFlags = (typeAndFlags & ~0x8) | (e ? 8 : 0); }
                void setRetain(const bool e)    { typeAndFlags = (typeAndFlags & ~0x1) | (e ? 1 : 0); }
                void setQoS(const uint8 e)      { typeAndFlags = (typeAndFlags & ~0x6) | (e < 3 ? (e << 1) : 0); }

                bool                check() const { return true; }
                static bool         check(const uint8 flag) { return true; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sHeader: (type PUBLISH, retain %d, QoS %d, dup %d)\n", (int)indent, "", isRetain(), getQoS(), isDup()); }
#endif


                FixedHeaderType(const uint8 flags = 0) : typeAndFlags(((uint8)PUBLISH << 4) | flags) {}
                FixedHeaderType(const bool dup, const uint8 QoS, const bool retain) : typeAndFlags(((uint8)PUBLISH << 4) | (dup ? 8 : 0) | (retain ? 1 : 0) | (QoS < 3 ? (QoS << 1) : 0)) {}
            };
            
            /** The possible header types */
            typedef FixedHeaderType<CONNECT,    0> ConnectHeader;
            typedef FixedHeaderType<CONNACK,    0> ConnectACKHeader;
            typedef FixedHeaderType<PUBLISH,    0> PublishHeader;
            typedef FixedHeaderType<PUBACK,     0> PublishACKHeader;
            typedef FixedHeaderType<PUBREC,     0> PublishReceivedHeader;
            typedef FixedHeaderType<PUBREL,     2> PublishReleasedHeader;
            typedef FixedHeaderType<PUBCOMP,    0> PublishCompletedHeader;
            typedef FixedHeaderType<SUBSCRIBE,  2> SubscribeHeader;
            typedef FixedHeaderType<SUBACK,     0> SubscribeACKHeader;
            typedef FixedHeaderType<UNSUBSCRIBE,2> UnsubscribeHeader;
            typedef FixedHeaderType<UNSUBACK,   0> UnsubscribeACKHeader;
            typedef FixedHeaderType<PINGREQ,    0> PingRequestHeader;
            typedef FixedHeaderType<PINGRESP,   0> PingACKHeader;
            typedef FixedHeaderType<DISCONNECT, 0> DisconnectHeader;
            typedef FixedHeaderType<AUTH,       0> AuthenticationHeader;
#pragma pack(pop)

            /** Simple check header code and packet size.
                @return error that will be detected with isError() or the number of bytes required for this packet */
            static inline uint32 checkHeader(const uint8 * buffer, const uint32 size, ControlPacketType * type = 0)
            {
                if (size < 2) return NotEnoughData;
                uint8 expectedFlags[] = { 0xF, 0, 0xF, 0, 0, 2, 0, 2, 0, 2, 0, 0, 0, 0, 0, 0};
                if ((*buffer >> 4) != PUBLISH && ((*buffer & 0xF) ^ expectedFlags[(*buffer>>4)])) return BadData;
                if (type) *type = (ControlPacketType)(*buffer >> 4);
                // Then read the VB header
                VBInt len;
                uint32 s = len.readFrom(buffer + 1, size - 1);
                if (isError(s)) return s;
                return (uint32)len + s + 1;
            }

            /** The known property types (section 2.2.2.2) */
            enum PropertyType
            {
                BadProperty             = 0,    //!< Does not exist in the standard, but useful to store a bad property type


                PayloadFormat           = 0x01, //!< Payload Format Indicator
                MessageExpiryInterval   = 0x02, //!< Message Expiry Interval
                ContentType             = 0x03, //!< Content Type
                ResponseTopic           = 0x08, //!< Response Topic
                CorrelationData         = 0x09, //!< Correlation Data
                SubscriptionID          = 0x0B, //!< Subscription Identifier
                SessionExpiryInterval   = 0x11, //!< Session Expiry Interval
                AssignedClientID        = 0x12, //!< Assigned Client Identifier
                ServerKeepAlive         = 0x13, //!< Server Keep Alive
                AuthenticationMethod    = 0x15, //!< Authentication Method
                AuthenticationData      = 0x16, //!< Authentication Data
                RequestProblemInfo      = 0x17, //!< Request Problem Information
                WillDelayInterval       = 0x18, //!< Will Delay Interval
                RequestResponseInfo     = 0x19, //!< Request Response Information
                ResponseInfo            = 0x1A, //!< Response Information
                ServerReference         = 0x1C, //!< Server Reference
                ReasonString            = 0x1F, //!< Reason String
                ReceiveMax              = 0x21, //!< Receive Maximum
                TopicAliasMax           = 0x22, //!< Topic Alias Maximum
                TopicAlias              = 0x23, //!< Topic Alias
                QoSMax                  = 0x24, //!< Maximum QoS
                RetainAvailable         = 0x25, //!< Retain Available
                UserProperty            = 0x26, //!< User Property
                PacketSizeMax           = 0x27, //!< Maximum Packet Size
                WildcardSubAvailable    = 0x28, //!< Wildcard Subscription Available
                SubIDAvailable          = 0x29, //!< Subscription Identifier Available
                SharedSubAvailable      = 0x2A, //!< Shared Subscription Available
                
                
                
                
                MaxUsedPropertyType,            //!< Used as a gatekeeper for the knowing the maximum value for the properties
            };

            namespace PrivateRegistry
            {
                enum { PropertiesCount = 27 };
                static const uint8 invPropertyMap[MaxUsedPropertyType] =
                {
                    PropertiesCount, // BadProperty,  
                     0, // PayloadFormat           ,
                     1, // MessageExpiryInterval   ,
                     2, // ContentType             ,
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                     3, // ResponseTopic           ,
                     4, // CorrelationData         ,
                    PropertiesCount, // BadProperty,  
                     5, // SubscriptionID          ,
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                     6, // SessionExpiryInterval   ,
                     7, // AssignedClientID        ,
                     8, // ServerKeepAlive         ,
                    PropertiesCount, // BadProperty,  
                     9, // AuthenticationMethod    ,
                    10, // AuthenticationData      ,
                    11, // RequestProblemInfo      ,
                    12, // WillDelayInterval       ,
                    13, // RequestResponseInfo     ,
                    14, // ResponseInfo            ,
                    PropertiesCount, // BadProperty,  
                    15, // ServerReference         ,
                    PropertiesCount, // BadProperty,  
                    PropertiesCount, // BadProperty,  
                    16, // ReasonString            ,
                    PropertiesCount, // BadProperty,  
                    17, // ReceiveMax              ,
                    18, // TopicAliasMax           ,
                    19, // TopicAlias              ,
                    20, // QoSMax                  ,
                    21, // RetainAvailable         ,
                    22, // UserProperty            ,
                    23, // PacketSizeMax           ,
                    24, // WildcardSubAvailable    ,
                    25, // SubIDAvailable          ,
                    26, // SharedSubAvailable      ,
                };

                /** Get the property name for a given property type */
                static const char * getPropertyName(const uint8 propertyType)
                {
                    static const char* propertyMap[PrivateRegistry::PropertiesCount] = 
                    { 
                        "PayloadFormat", "MessageExpiryInterval", "ContentType", "ResponseTopic", "CorrelationData",
                        "SubscriptionID", "SessionExpiryInterval", "AssignedClientID", "ServerKeepAlive", 
                        "AuthenticationMethod", "AuthenticationData", "RequestProblemInfo", "WillDelayInterval", 
                        "RequestResponseInfo", "ResponseInfo", "ServerReference", "ReasonString", "ReceiveMax", 
                        "TopicAliasMax", "TopicAlias", "QoSMax", "RetainAvailable", "UserProperty", "PacketSizeMax", 
                        "WildcardSubAvailable", "SubIDAvailable", "SharedSubAvailable", 
                    };
                    if (propertyType >= MaxUsedPropertyType) return 0;
                    uint8 index = PrivateRegistry::invPropertyMap[propertyType];
                    if (index == PrivateRegistry::PropertiesCount) return 0;
                    return propertyMap[index];
                }
            }
            
            
            /** This is a simple property header that's common to all properties */
            struct PropertyBase : public SerializableWithSuicide
            {
                /** While we should support a variable length property type, there is no property type allowed above 127 for now, so let's resume to a single uint8 */
                const uint8 type;
                /** The property type */
                PropertyBase(const PropertyType type) : type((uint8)type) {}
                /** Clone the property */
                virtual PropertyBase * clone() const = 0;
                /** Required destructor */
                virtual ~PropertyBase() {}
            };
            
            /** The base of all PropertyView. They are mapped on an existing buffer and are not allocating anything. */
            struct PropertyBaseView : public MemMappedVisitor
            {
                /** While we should support a variable length property type, there is no property type allowed above 127 for now, so let's resume to a single uint8 */
                const uint8 type;
                /** The property type */
                PropertyBaseView(const PropertyType type) : type((uint8)type) {}
                /** Required destructor */
                virtual ~PropertyBaseView() {}
            };



            template <typename T>
            struct MMProp
            {
                static MemMappedVisitor & getInstance()
                {
                    static T visitor; return visitor;
                }
            };

            /** A registry used to store the mapping between properties and their visitor */
            class MemMappedPropertyRegistry
            {
                MemMappedVisitor * properties[PrivateRegistry::PropertiesCount];
                
            public:
                static MemMappedPropertyRegistry & getInstance()
                {
                    static MemMappedPropertyRegistry registry;
                    return registry;
                }

                const char * getPropertyName(const uint8 propertyType) { return PrivateRegistry::getPropertyName(propertyType); }

                MemMappedVisitor * getVisitorForProperty(const uint8 propertyType)
                {
                    if (propertyType >= MaxUsedPropertyType) return 0;

                    uint8 index = PrivateRegistry::invPropertyMap[propertyType];
                    if (index == PrivateRegistry::PropertiesCount) return 0;
                    return properties[index];
                }

            private:
                MemMappedPropertyRegistry()
                {
                    // Register all properties now
                    properties[ 0] = &MMProp< PODVisitor<uint8>              >::getInstance(); // PayloadFormat         
                    properties[ 1] = &MMProp< LittleEndianPODVisitor<uint32> >::getInstance(); // MessageExpiryInterval 
                    properties[ 2] = &MMProp< DynamicStringView              >::getInstance(); // ContentType           
                    properties[ 3] = &MMProp< DynamicStringView              >::getInstance(); // ResponseTopic         
                    properties[ 4] = &MMProp< DynamicBinDataView             >::getInstance(); // CorrelationData       
                    properties[ 5] = &MMProp< MappedVBInt                    >::getInstance(); // SubscriptionID        
                    properties[ 6] = &MMProp< LittleEndianPODVisitor<uint32> >::getInstance(); // SessionExpiryInterval 
                    properties[ 7] = &MMProp< DynamicStringView              >::getInstance(); // AssignedClientID      
                    properties[ 8] = &MMProp< LittleEndianPODVisitor<uint16> >::getInstance(); // ServerKeepAlive       
                    properties[ 9] = &MMProp< DynamicStringView              >::getInstance(); // AuthenticationMethod  
                    properties[10] = &MMProp< DynamicBinDataView             >::getInstance(); // AuthenticationData    
                    properties[11] = &MMProp< PODVisitor<uint8>              >::getInstance(); // RequestProblemInfo    
                    properties[12] = &MMProp< LittleEndianPODVisitor<uint32> >::getInstance(); // WillDelayInterval     
                    properties[13] = &MMProp< PODVisitor<uint8>              >::getInstance(); // RequestResponseInfo   
                    properties[14] = &MMProp< DynamicStringView              >::getInstance(); // ResponseInfo          
                    properties[15] = &MMProp< DynamicStringView              >::getInstance(); // ServerReference       
                    properties[16] = &MMProp< DynamicStringView              >::getInstance(); // ReasonString          
                    properties[17] = &MMProp< LittleEndianPODVisitor<uint16> >::getInstance(); // ReceiveMax            
                    properties[18] = &MMProp< LittleEndianPODVisitor<uint16> >::getInstance(); // TopicAliasMax         
                    properties[19] = &MMProp< LittleEndianPODVisitor<uint16> >::getInstance(); // TopicAlias            
                    properties[20] = &MMProp< PODVisitor<uint8>              >::getInstance(); // QoSMax                
                    properties[21] = &MMProp< PODVisitor<uint8>              >::getInstance(); // RetainAvailable       
                    properties[22] = &MMProp< DynamicStringPairView          >::getInstance(); // UserProperty          
                    properties[23] = &MMProp< LittleEndianPODVisitor<uint32> >::getInstance(); // PacketSizeMax         
                    properties[24] = &MMProp< PODVisitor<uint8>              >::getInstance(); // WildcardSubAvailable  
                    properties[25] = &MMProp< PODVisitor<uint8>              >::getInstance(); // SubIDAvailable        
                    properties[26] = &MMProp< PODVisitor<uint8>              >::getInstance(); // SharedSubAvailable    
                }
            };

            /** The link between the property type and its possible value follow section 2.2.2.2 */
            template <typename T>
            struct Property : public PropertyBase
            {
                /** The property value, depends on the type */
                T           value;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + sizeof(value); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; T v = BigEndian(value); memcpy(buffer+1, &v, sizeof(value)); return sizeof(value) + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < sizeof(value)+1) return NotEnoughData;
                    memcpy(&value, buffer+1, sizeof(value));
                    value = BigEndian(value);
                    return sizeof(value) + 1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    out += MQTTStringPrintf("%*s", indent+2, ""); out += value; out += "\n"; 
                }
#endif

                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }

                /** The default constructor */
                Property(const PropertyType type, T value = 0) : PropertyBase(type), value(value) {}
            };

            /** A property that's allocated on the stack. It never suicide itself. */
            template <typename T>
            struct StackProperty : public Property<T>
            {
                /** The default constructor */
                StackProperty(const PropertyType type, T value = 0) : Property<T>(type, value) {}
                /** Clone this property by a non stack based property */
                PropertyBase * clone() const { return new Property<T>((PropertyType)this->type, this->value); }
                /** Don't ever suicide! */
                void suicide() {}
            };
            
            template<>
            struct Property<DynamicString> : public PropertyBase
            {
                /** The property value, depends on the type */
                DynamicString       value;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 3) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif
                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }

                /** The default constructor */
                Property(const PropertyType type, const DynamicString value = "") : PropertyBase(type), value(value) {}
            };

            template<>
            struct Property<DynamicBinaryData> : public PropertyBase
            {
                /** The property value, depends on the type */
                DynamicBinaryData   value;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 3) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif

                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }

                /** The default constructor */
                Property(const PropertyType type, const DynamicBinaryData value = 0) : PropertyBase(type), value(value) {}
            };

            template<>
            struct Property<DynamicStringPair> : public PropertyBase
            {
                /** The property value, depends on the type */
                DynamicStringPair   value;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 5) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif
                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }

                /** The default constructor */
                Property(const PropertyType type, const DynamicStringPair value) : PropertyBase(type), value(value) {}
            };


            template<>
            struct Property<DynamicStringView> : public PropertyBase
            {
                /** The property value, depends on the type */
                DynamicStringView       value;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 3) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif
                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }

                /** The default constructor */
                Property(const PropertyType type, const DynamicStringView value = "") : PropertyBase(type), value(value) {}
            };

            template<>
            struct Property<DynamicBinDataView> : public PropertyBase
            {
                /** The property value, depends on the type */
                DynamicBinDataView   value;
            
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 3) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif
                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }
                /** The default constructor */
                Property(const PropertyType type, const DynamicBinDataView value = 0) : PropertyBase(type), value(value) {}
            };

            template<>
            class Property<DynamicStringPairView> : public PropertyBase
            {
                /** The property value, depends on the type */
                DynamicStringPairView   value;

            public:
                /** Get the value as dynamic string */
                const DynamicStringPairView & getValue() const { return value; }
                /** Set the value */
                void setValue(const DynamicStringPairView & v) { value = v; }

                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 5) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif

                /** The default constructor */
                Property(const PropertyType type, const DynamicStringPairView value) : PropertyBase(type), value(value) {}
            };

            
            template<>
            struct Property<VBInt> : public PropertyBase
            {
                /** The property value, depends on the type */
                VBInt               value;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return sizeof(type) + value.getSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long, and at worst very large (use getSize to figure out the required size).
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { buffer[0] = type; uint32 o = value.copyInto(buffer+1); return o + 1; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or BadData upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if ((buffer[0] & 0x80) || buffer[0] != type) return BadData;
                    if (bufLength < 2) return NotEnoughData;
                    uint32 o = value.readFrom(buffer+1, bufLength - 1);
                    if (isError(o)) return o;
                    return o+1;
                }
                /** Check if this property is valid */
                bool check() const { return type < 0x80 && value.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sType %s\n", indent, "", PrivateRegistry::getPropertyName(type));
                    value.dump(out, indent + 2); 
                }
#endif

                /** Clone this property */
                PropertyBase * clone() const { return new Property((PropertyType)type, value); }

                /** The default constructor */
                Property(const PropertyType type, const uint32 value = 0) : PropertyBase(type), value(value) {}
            };
            
            /** The deserialization registry for properties */
            struct PropertyRegistry
            {
                /** The function used to create a new instance of a property */
                typedef PropertyBase * (*InstantiateFunc)();
                
                /** The creator method array */
                InstantiateFunc unserializeFunc[MaxUsedPropertyType];
                /** Register a property to this registry */
                void registerProperty(PropertyType type, InstantiateFunc func) { unserializeFunc[type] = func; }
                /** Unserialize the given buffer, the buffer must point to the property to unserialize.
                    @param buffer       A pointer to a buffer that's bufLength bytes long
                    @param bufLength    Length of the buffer in bytes
                    @param output       On output, will be allocated to a Serializable (the expected property type)
                    @return the used number of bytes in the buffer, or a LocalError upon error */
                uint32 unserialize(const uint8 * buffer, uint32 bufLength, PropertyBase *& output)
                {
                    if (bufLength < 1 || !buffer) return NotEnoughData;
                    uint8 type = buffer[0];
                    if (type >= MaxUsedPropertyType) return BadData;
                    InstantiateFunc f = unserializeFunc[type];
                    if (!f) return BadData;
                    // Instantiate the right property
                    output = (*f)();
                    if (!output) return BadData;
                    return output->readFrom(buffer, bufLength);
                }
                /** Get an instance of this object */
                static PropertyRegistry & getInstance() { static PropertyRegistry reg; return reg; }
            };


            template <PropertyType type, typename T>
            struct TypedProperty : public Property<T>
            {
                enum { Type = type };
                static PropertyBase * allocateProp() { return new Property<T>(type, T()) ; }
                TypedProperty(const T & value) : Property<T>(type, value) {}
            };

            /** Usual autoregister the property creation function */
            template <typename T>
            struct AutoRegisterProperty
            {
                AutoRegisterProperty() { PropertyRegistry::getInstance().registerProperty((PropertyType)T::Type, &T::allocateProp); }
            };


            /** Then declare all properties now, based on Table 2-4 */
            typedef TypedProperty<PayloadFormat, uint8>                     PayloadFormatProp;
            typedef TypedProperty<MessageExpiryInterval, uint32>            MessageExpiryIntervalProp;
            typedef TypedProperty<ContentType, DynamicString>               ContentTypeProp;
            typedef TypedProperty<ResponseTopic, DynamicString>             ResponseTopicProp;
            typedef TypedProperty<CorrelationData, DynamicBinaryData>       CorrelationDataProp;
            typedef TypedProperty<SubscriptionID, VBInt>                    SubscriptionIDProp;
            typedef TypedProperty<SessionExpiryInterval, uint32>            SessionExpiryIntervalProp;
            typedef TypedProperty<AssignedClientID, DynamicString>          AssignedClientIDProp;
            typedef TypedProperty<ServerKeepAlive, uint16>                  ServerKeepAliveProp;
            typedef TypedProperty<AuthenticationMethod, DynamicString>      AuthenticationMethodProp;
            typedef TypedProperty<AuthenticationData, DynamicBinaryData>    AuthenticationDataProp;
            typedef TypedProperty<RequestProblemInfo, uint8>                RequestProblemInfoProp;
            typedef TypedProperty<WillDelayInterval, uint32>                WillDelayIntervalProp;
            typedef TypedProperty<RequestResponseInfo, uint8>               RequestResponseInfoProp;
            typedef TypedProperty<ResponseInfo, DynamicString>              ResponseInfoProp;
            typedef TypedProperty<ServerReference, DynamicString>           ServerReferenceProp;
            typedef TypedProperty<ReasonString, DynamicString>              ReasonStringProp;
            typedef TypedProperty<ReceiveMax, uint16>                       ReceiveMaxProp;
            typedef TypedProperty<TopicAliasMax, uint16>                    TopicAliasMaxProp;
            typedef TypedProperty<TopicAlias, uint16>                       TopicAliasProp;
            typedef TypedProperty<QoSMax, uint8>                            QoSMaxProp;
            typedef TypedProperty<RetainAvailable, uint8>                   RetainAvailableProp;
            typedef TypedProperty<UserProperty, DynamicStringPair>          UserPropertyProp;
            typedef TypedProperty<PacketSizeMax, uint32>                    PacketSizeMaxProp;
            typedef TypedProperty<WildcardSubAvailable, uint8>              WildcardSubAvailableProp;
            typedef TypedProperty<SubIDAvailable, uint8>                    SubIDAvailableProp;
            typedef TypedProperty<SharedSubAvailable, uint8>                SharedSubAvailableProp;
            
            /** This needs to be done once, at least */
            static inline void registerAllProperties()
            {
                static bool registryDoneAlready = false;
                if (!registryDoneAlready)
                {
                    AutoRegisterProperty<PayloadFormatProp>         PayloadFormatProp_reg;
                    AutoRegisterProperty<MessageExpiryIntervalProp> MessageExpiryIntervalProp_reg;
                    AutoRegisterProperty<ContentTypeProp>           ContentTypeProp_reg;
                    AutoRegisterProperty<ResponseTopicProp>         ResponseTopicProp_reg;
                    AutoRegisterProperty<CorrelationDataProp>       CorrelationDataProp_reg;
                    AutoRegisterProperty<SubscriptionIDProp>        SubscriptionIDProp_reg;
                    AutoRegisterProperty<SessionExpiryIntervalProp> SessionExpiryIntervalProp_reg;
                    AutoRegisterProperty<AssignedClientIDProp>      AssignedClientIDProp_reg;
                    AutoRegisterProperty<ServerKeepAliveProp>       ServerKeepAliveProp_reg;
                    AutoRegisterProperty<AuthenticationMethodProp>  AuthenticationMethodProp_reg;
                    AutoRegisterProperty<AuthenticationDataProp>    AuthenticationDataProp_reg;
                    AutoRegisterProperty<RequestProblemInfoProp>    RequestProblemInfoProp_reg;
                    AutoRegisterProperty<WillDelayIntervalProp>     WillDelayIntervalProp_reg;
                    AutoRegisterProperty<RequestResponseInfoProp>   RequestResponseInfoProp_reg;
                    AutoRegisterProperty<ResponseInfoProp>          ResponseInfoProp_reg;
                    AutoRegisterProperty<ServerReferenceProp>       ServerReferenceProp_reg;
                    AutoRegisterProperty<ReasonStringProp>          ReasonStringProp_reg;
                    AutoRegisterProperty<ReceiveMaxProp>            ReceiveMaxProp_reg;
                    AutoRegisterProperty<TopicAliasMaxProp>         TopicAliasMaxProp_reg;
                    AutoRegisterProperty<TopicAliasProp>            TopicAliasProp_reg;
                    AutoRegisterProperty<QoSMaxProp>                QoSMaxProp_reg;
                    AutoRegisterProperty<RetainAvailableProp>       RetainAvailableProp_reg;
                    AutoRegisterProperty<UserPropertyProp>          UserPropertyProp_reg;
                    AutoRegisterProperty<PacketSizeMaxProp>         PacketSizeMaxProp_reg;
                    AutoRegisterProperty<WildcardSubAvailableProp>  WildcardSubAvailableProp_reg;
                    AutoRegisterProperty<SubIDAvailableProp>        SubIDAvailableProp_reg;
                    AutoRegisterProperty<SharedSubAvailableProp>    SharedSubAvailableProp_reg;
                    registryDoneAlready = true;
                }
            }


            /** Property list */
            struct PropertyListNode
            {
                PropertyBase * property;
                PropertyListNode * next;
                
                /** Append a new property to this list */
                void append(PropertyBase * prop) { PropertyListNode * u = this; while(u->next) u = u->next; u->next = new PropertyListNode(prop); }
                /** Count the number of element in this list */
                uint32 count() const { const PropertyListNode * u = this; uint32 c = 1; while(u->next) { u = u->next; c++; } return c; }
                /** Check all properties in this list are valid */
                bool check() const { const PropertyListNode * u = this; while (u->next) { if (!u->property->check()) return false; u = u->next; } return true; }
                /** Clone a single node here (not the complete list) */
                PropertyListNode * clone() const { return new PropertyListNode(property->clone()); }
                
                PropertyListNode(PropertyBase * property) : property(property), next(0) {}
                ~PropertyListNode() { delete0(next); property->suicide(); property = 0; }
            };
            
            /** The allowed properties for each control packet type.
                This is used externally to allow generic code to be written */
            template <PropertyType type> struct ExpectedProperty { enum { AllowedMask = 0 }; };
            template <> struct ExpectedProperty<PayloadFormat>           { enum { AllowedMask = (1<<(uint8)PUBLISH) | 1 }; };
            template <> struct ExpectedProperty<MessageExpiryInterval>   { enum { AllowedMask = (1<<(uint8)PUBLISH) | 1 }; };
            template <> struct ExpectedProperty<ContentType>             { enum { AllowedMask = (1<<(uint8)PUBLISH) | 1 }; };
            template <> struct ExpectedProperty<ResponseTopic>           { enum { AllowedMask = (1<<(uint8)PUBLISH) | 1 }; };
            template <> struct ExpectedProperty<CorrelationData>         { enum { AllowedMask = (1<<(uint8)PUBLISH) | 1 }; };
            template <> struct ExpectedProperty<TopicAlias>              { enum { AllowedMask = (1<<(uint8)PUBLISH) }; };
            template <> struct ExpectedProperty<WillDelayInterval>       { enum { AllowedMask = 1 }; };
            template <> struct ExpectedProperty<SubscriptionID>          { enum { AllowedMask = (1<<(uint8)PUBLISH) | (1<<(uint8)SUBSCRIBE) }; };
            template <> struct ExpectedProperty<SessionExpiryInterval>   { enum { AllowedMask = (1<<(uint8)CONNECT) | (1<<(uint8)CONNACK) | (1<<(uint8)DISCONNECT) }; };
            template <> struct ExpectedProperty<AuthenticationMethod>    { enum { AllowedMask = (1<<(uint8)CONNECT) | (1<<(uint8)CONNACK) | (1<<(uint8)AUTH) }; };
            template <> struct ExpectedProperty<AuthenticationData>      { enum { AllowedMask = (1<<(uint8)CONNECT) | (1<<(uint8)CONNACK) | (1<<(uint8)AUTH) }; };
            template <> struct ExpectedProperty<ReceiveMax>              { enum { AllowedMask = (1<<(uint8)CONNECT) | (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<TopicAliasMax>           { enum { AllowedMask = (1<<(uint8)CONNECT) | (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<PacketSizeMax>           { enum { AllowedMask = (1<<(uint8)CONNECT) | (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<RequestProblemInfo>      { enum { AllowedMask = (1<<(uint8)CONNECT) }; };
            template <> struct ExpectedProperty<RequestResponseInfo>     { enum { AllowedMask = (1<<(uint8)CONNECT) }; };
            template <> struct ExpectedProperty<AssignedClientID>        { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<ServerKeepAlive>         { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<QoSMax>                  { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<RetainAvailable>         { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<WildcardSubAvailable>    { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<SubIDAvailable>          { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<SharedSubAvailable>      { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<ResponseInfo>            { enum { AllowedMask = (1<<(uint8)CONNACK) }; };
            template <> struct ExpectedProperty<ServerReference>         { enum { AllowedMask = (1<<(uint8)CONNACK) | (1<<(uint8)DISCONNECT) }; };
            template <> struct ExpectedProperty<ReasonString>            { enum { AllowedMask = (1<<(uint8)CONNACK) | (1<<(uint8)PUBACK) | (1<<(uint8)PUBREC) | (1<<(uint8)PUBREL) | (1<<(uint8)PUBCOMP) | (1<<(uint8)SUBACK) | (1<<(uint8)UNSUBACK) | (1<<(uint8)DISCONNECT) | (1<<(uint8)AUTH) }; };
            template <> struct ExpectedProperty<UserProperty>            { enum { AllowedMask = 0xFFFF }; };
            /** Check if the given property is allowed in the given control packet type in O(1) */
            static inline bool isAllowedProperty(const PropertyType type, const ControlPacketType ctype)
            {   // This takes 82 bytes of program memory by allowing O(1) in property validity, compared to O(N) search and duplicated code everywhere.
                static uint16 allowedProperties[MaxUsedPropertyType] =
                {
                    ExpectedProperty<(PropertyType) 1>::AllowedMask, ExpectedProperty<(PropertyType) 2>::AllowedMask, ExpectedProperty<(PropertyType) 3>::AllowedMask, ExpectedProperty<(PropertyType) 4>::AllowedMask, ExpectedProperty<(PropertyType) 5>::AllowedMask, ExpectedProperty<(PropertyType) 6>::AllowedMask,
                    ExpectedProperty<(PropertyType) 7>::AllowedMask, ExpectedProperty<(PropertyType) 8>::AllowedMask, ExpectedProperty<(PropertyType) 9>::AllowedMask, ExpectedProperty<(PropertyType)10>::AllowedMask, ExpectedProperty<(PropertyType)11>::AllowedMask, ExpectedProperty<(PropertyType)12>::AllowedMask,
                    ExpectedProperty<(PropertyType)13>::AllowedMask, ExpectedProperty<(PropertyType)14>::AllowedMask, ExpectedProperty<(PropertyType)15>::AllowedMask, ExpectedProperty<(PropertyType)16>::AllowedMask, ExpectedProperty<(PropertyType)17>::AllowedMask, ExpectedProperty<(PropertyType)18>::AllowedMask,
                    ExpectedProperty<(PropertyType)19>::AllowedMask, ExpectedProperty<(PropertyType)20>::AllowedMask, ExpectedProperty<(PropertyType)21>::AllowedMask, ExpectedProperty<(PropertyType)22>::AllowedMask, ExpectedProperty<(PropertyType)23>::AllowedMask, ExpectedProperty<(PropertyType)24>::AllowedMask,
                    ExpectedProperty<(PropertyType)25>::AllowedMask, ExpectedProperty<(PropertyType)26>::AllowedMask, ExpectedProperty<(PropertyType)27>::AllowedMask, ExpectedProperty<(PropertyType)28>::AllowedMask, ExpectedProperty<(PropertyType)29>::AllowedMask, ExpectedProperty<(PropertyType)30>::AllowedMask,
                    ExpectedProperty<(PropertyType)31>::AllowedMask, ExpectedProperty<(PropertyType)32>::AllowedMask, ExpectedProperty<(PropertyType)33>::AllowedMask, ExpectedProperty<(PropertyType)34>::AllowedMask, ExpectedProperty<(PropertyType)35>::AllowedMask, ExpectedProperty<(PropertyType)36>::AllowedMask,
                    ExpectedProperty<(PropertyType)37>::AllowedMask, ExpectedProperty<(PropertyType)38>::AllowedMask, ExpectedProperty<(PropertyType)39>::AllowedMask, ExpectedProperty<(PropertyType)40>::AllowedMask, ExpectedProperty<(PropertyType)41>::AllowedMask, ExpectedProperty<(PropertyType)42>::AllowedMask,
                };
                if (!type || type >= MaxUsedPropertyType) return 0;
                return (allowedProperties[(int)type - 1] & (1<<(uint8)ctype)) > 0;
            }

            /** The property structure (section 2.2.2) */
            struct Properties : public Serializable
            {
                /** The properties length (can be 0) (this only counts the following members) */
                VBInt length;
                /** The properties set */
                PropertyListNode * head;

                /** Get the i-th property */
                const PropertyBase * getProperty(size_t index) const { const PropertyListNode * u = head; while (u && index--) u = u->next; return u ? u->property : 0; }
                /** Get the i-th property of the given type */
                const PropertyBase * getProperty(const PropertyType type, size_t index = 0) const
                {
                    PropertyListNode * u = head;
                    while (u)
                    {
                        if (u->property && u->property->type == type)
                           if (index-- == 0) return u->property;
                        u = u->next;
                    }
                    return 0;
                }
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return length.getSize() + (uint32)length; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    uint32 o = length.copyInto(buffer);
                    PropertyListNode * c = head;
                    while (c && c->property) { o += c->property->copyInto(buffer + o); c = c->next; }
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    uint32 o = length.readFrom(buffer, bufLength);
                    if (isError(o)) return o;
                    if ((uint32)length > bufLength - length.getSize()) return NotEnoughData;
                    delete0(head);
                    buffer += o; bufLength -= o;
                    PropertyBase * property = 0;
                    uint32 cumSize = (uint32)length;
                    while (cumSize)
                    {
                        uint32 s = PropertyRegistry::getInstance().unserialize(buffer, cumSize, property);
                        if (isError(s)) return s;
                        if (!head) head = new PropertyListNode(property);
                        else head->append(property);
                        buffer += s; cumSize -= s;
                        o += s;
                    }
                    return o;
                }
                /** Check if this property is valid */
                bool check() const { return length.check() && head ? head->check() : true; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sProperties with length ", (int)indent, ""); length.dump(out, 0);
                    if (!(uint32)length) return;
                    PropertyListNode * c = head;
                    while (c && c->property) {
                        c->property->dump(out, indent + 2);
                        c = c->next;
                    }
                }
#endif
                /** Check if the properties are compatible for the given packet type */
                bool checkPropertiesFor(const ControlPacketType type) const
                {
                    if (!check()) return false;
                    PropertyListNode * u = head;
                    while (u) { if (!isAllowedProperty((PropertyType)u->property->type, type)) return false; u = u->next; }
                    return true;
                }

                /** Append a property to this list
                    @param property     A pointer to a new allocated property to append to this list that's owned
                    @return true upon successful append, false upon error (the pointer is not owned in that case) */
                bool append(PropertyBase * property)
                {
                    VBInt l((uint32)length + property->getSize());
                    if (!l.check()) return false;
                    length = l;
                    if (head) head->append(property);
                    else head = new PropertyListNode(property);
                    return true;
                }
                /** Swap the list with another properties list */
                void swap(Properties & other)
                {
                    PropertyListNode * n = head;
                    head = other.head;
                    other.head = n;
                    uint32 v = (uint32)length;
                    length = (uint32)other.length;
                    other.length = v;
                }
                
                /** Build an empty property list */
                Properties() : head(0) {}
                /** Copy construction */
                Properties(const Properties & other) : length(other.length), head(0)
                {
                    const PropertyListNode * n = other.head;
                    PropertyListNode * & m = head;
                    while (n)
                    {
                        m = n->clone();
                        m = m->next;
                        n = n->next;
                    }
                }
#if HasCPlusPlus11 == 1
                /** Move constructor (to be preferred) */
                Properties(Properties && other) : length(std::move(other.length)), head(std::move(other.head)) {}
#endif
                /** Build a property list starting with the given property that's owned.
                    @param firstProperty    A pointer on a new allocated Property that's owned by this list */
                Properties(PropertyBase * firstProperty) 
                    : length(firstProperty->getSize()), head(new PropertyListNode(firstProperty)) {}
                ~Properties() { delete0(head); }
            };


            /** A read-only view off property extracted from a packet (section 2.2.2).
                Unlike the Properties class above that's able to add and parse properties, this
                one only parse properties but never allocate anything on the heap.

                The idea here is to parse properties on the fly, one by one and let the client code 
                perform fetching the information it wants from them.

                Typically, you'll use this class like this:
                @code
                    PropertyView v;
                    uint32 r = v.readFrom(buffer, bufLength);
                    if (isError(r)) return BadData;

                    uint32 offset = 0;
                    PropertyType type = BadProperty;
                    MemMappedVisitor * visitor = v.getProperty(type, offset);
                    while (visitor)
                    {
                        if (type == DesiredProperty)
                        {
                            DynamicStringView * view = static_cast<DynamicStringView *>(visitor);
                            // Do something with view
                        }
                        else if (type == SomeOtherProperty)
                        {
                            PODVisitor<uint8> * pod = static_cast<PODVisitor<uint8> *>(visitor);
                            uint8 value = pod->getValue(); // Do something with value 
                        }
                        visitor = v.getProperty(type, offset);
                    }
                @endcode */
            struct PropertiesView : public Serializable
            {
                /** The properties length (can be 0) (this only counts the following members) */
                VBInt length;
                /** The given input buffer */
                const uint8 * buffer;

                /** Fetch the i-th property
                    @param type     On output, will be filled with the given type or BadProperty if none found
                    @param offset   On output, will be filled to the offset in bytes to the next property
                    @return A pointer on a static instance (stored on TLS if WantThreadLocalStorage is defined) or 0 upon error */
                MemMappedVisitor * getProperty(PropertyType & type, uint32 & offset) const
                {
                    type = BadProperty;
                    if (offset >= (uint32)length || !buffer) return 0;
                    // Deduce property type from the given byte
                    uint8 t = buffer[offset];
                    MemMappedVisitor * visitor = MemMappedPropertyRegistry::getInstance().getVisitorForProperty(t);
                    if (!visitor) return 0;
                    type = (PropertyType)t;
                    // Then visit the property now
                    uint32 r = visitor->acceptBuffer(&buffer[offset + 1], (uint32)length - offset - 1);
                    if (isError(r)) return 0;
                    offset += r + 1;
                    return visitor;
                }
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return length.getSize() + (uint32)length; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * _buffer) const
                {
                    uint32 o = length.copyInto(_buffer);
                    memcpy(_buffer + o, buffer, (uint32)length);
                    return o + (uint32)length;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * _buffer, uint32 bufLength)
                {
                    uint32 o = length.readFrom(_buffer, bufLength);
                    if (isError(o)) return o;
                    if ((uint32)length > bufLength - length.getSize()) return NotEnoughData;
                    buffer = _buffer + o;
                    return o + (uint32)length;
                }
                /** Check if this property is valid */
                bool check() const { return length.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sProperties with length ", (int)indent, ""); length.dump(out, 0);
                    if (!(uint32)length) return;
                    PropertyType type; uint32 offset = 0;
                    while (MemMappedVisitor * visitor = getProperty(type, offset))
                    {
                        out += MQTTStringPrintf("%*sType %s\n", indent+2, "", PrivateRegistry::getPropertyName(type));
                        visitor->dump(out, indent + 4);
                    }
                }
#endif                
                /** Check if the properties are compatible for the given packet type */
                bool checkPropertiesFor(const ControlPacketType type) const
                {
#if MQTTAvoidValidation != 1
                    if (!check()) return false;
                    uint32 o = 0;
                    PropertyType t = BadProperty; 
                    while (getProperty(t, o))
                    {
                        if (!isAllowedProperty(t, type)) return false;
                    }
#endif
                    return true;
                }                
                
                /** Build an empty property list */
                PropertiesView() : buffer(0) {}
                /** Copy construction */
                PropertiesView(const PropertiesView & other) : length(other.length), buffer(other.buffer)
                {}
#if HasCPlusPlus11 == 1
                /** Move constructor (to be preferred) */
                PropertiesView(PropertiesView && other) : length(std::move(other.length)), buffer(std::move(other.buffer)) {}
#endif
            };
            
            /** The possible value for retain handling in subscribe packet */
            enum RetainHandling
            {
                GetRetainedMessageAtSubscriptionTime        = 0,    //!< Get the retained message at subscription time
                GetRetainedMessageForNewSubscriptionOnly    = 1,    //!< Get the retained message only for new subscription
                NoRetainedMessage                           = 2,    //!< Don't get retained message at all
            };

            /** The possible Quality Of Service values */
            enum QualityOfServiceDelivery
            {
                AtMostOne                           = 0,    //!< At most one delivery (unsecure sending)
                AtLeastOne                          = 1,    //!< At least one delivery (could have retransmission)
                ExactlyOne                          = 2,    //!< Exactly one delivery (longer to send) 
            };

 #pragma pack(push, 1)
            /** The subscribe topic list */
            struct SubscribeTopic : public SerializableWithSuicide
            {
                /** The subscribe topic */
                DynString   topic;

                union
                {
                    /** The subscribe option */
                    uint8 option;
                    struct
                    {
#if IsBigEndian == 1
                        /** Reserved bits, should be 0 */
                        uint8 reserved : 2;
                        /** The retain policy flag */
                        uint8 retainHandling : 2;
                        /** The retain as published flag */
                        uint8 retainAsPublished : 1;
                        /** The non local flag */
                        uint8 nonLocal : 1;
                        /** The QoS flag */
                        uint8 QoS : 2;
#else
                        /** The QoS flag */
                        uint8 QoS : 2;
                        /** The non local flag */
                        uint8 nonLocal : 1;
                        /** The retain as published flag */
                        uint8 retainAsPublished : 1;
                        /** The retain policy flag */
                        uint8 retainHandling : 2;
                        /** Reserved bits, should be 0 */
                        uint8 reserved : 2;
#endif
                    };
                };
                /** If there is a next topic, we need to access it */
                SubscribeTopic * next;
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return topic.getSize() + 1 + (next ? next->getSize() : 0); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    uint32 o = topic.copyInto(buffer);
                    buffer += o;
                    buffer[0] = option; o++;
                    if (next) o += next->copyInto(buffer+1);
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (next) next->suicide(); 
                    next = 0;
                    uint32 o = 0, s = topic.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    buffer += s; bufLength -= s; o += s;
                    if (!bufLength) return NotEnoughData;
                    option = buffer[0]; buffer++; bufLength--; o++;
                    if (bufLength)
                    {
                        next = new SubscribeTopic();
                        s = next->readFrom(buffer, bufLength);
                        if (isError(s)) return s;
                        o += s;
                    }
                    return o;
                }
                /** Check if this property is valid */
                bool check() const { return reserved == 0 && retainAsPublished != 3 && QoS != 3 && topic.check() && (next ? next->check() : true); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sSubscribe (QoS %d, nonLocal %d, retainAsPublished %d, retainHandling %d): ", (int)indent, "", QoS, nonLocal, retainAsPublished, retainHandling); topic.dump(out, indent); 
                    if (next) next->dump(out, indent);
                }
#endif

                /** Append a subscribe topic to the end of this list */
                void append(SubscribeTopic * newTopic) { SubscribeTopic ** end = &next; while(*end) { end = &(*end)->next; } *end = newTopic; }
                /** Count the number of topic */
                uint32 count() const { uint32 c = 1; const SubscribeTopic * p = next; while (p) { c++; p = p->next; } return c; }

                /** Default constructor */
                SubscribeTopic() : option(0), next(0) {}
                /** Full constructor */
                SubscribeTopic(const DynString & topic, const uint8 retainHandling, const bool retainAsPublished, const bool nonLocal, const uint8 QoS)
                    : topic(topic), option(0), next(0) { this->retainHandling = retainHandling; this->retainAsPublished = retainAsPublished ? 1 : 0; this->nonLocal = nonLocal ? 1:0; this->QoS = QoS; }
                /** Obvious destructor */
                ~SubscribeTopic() { if (next) next->suicide(); next = 0; }
            };

            /** A stack based subscribe topic */
            struct StackSubscribeTopic : public SubscribeTopic
            {
                /** Make sure to call the next item as it might be heap allocated */
                void suicide() { if (next) next->suicide(); }

                StackSubscribeTopic(const DynString & topic, const uint8 retainHandling, const bool retainAsPublished, const bool nonLocal, const uint8 QoS)
                    : SubscribeTopic(topic, retainHandling, retainAsPublished, nonLocal, QoS) {}
            };
            
#pragma pack(pop)
            /** The unsubscribe topic list */
            struct UnsubscribeTopic : public SerializableWithSuicide
            {
                /** The unsubscribe topic */
                DynString           topic;
                /** If there is a next topic, we need to access it */
                UnsubscribeTopic *  next;

                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return topic.getSize() + (next ? next->getSize() : 0); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    uint32 o = topic.copyInto(buffer);
                    buffer += o;
                    if (next) o += next->copyInto(buffer);
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (next) next->suicide(); 
                    next = 0;
                    uint32 o = 0, s = topic.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    buffer += s; bufLength -= s; o += s;
                    if (bufLength)
                    {
                        next = new UnsubscribeTopic();
                        s = next->readFrom(buffer, bufLength);
                        if (isError(s)) return s;
                        o += s;
                    }
                    return o;
                }
                /** Check if this property is valid */
                bool check() const { return topic.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sUnsubscribe: ", (int)indent, ""); topic.dump(out, indent); 
                    if (next) next->dump(out, indent);
                }
#endif

                /** Append a subscribe topic to the end of this list */
                void append(UnsubscribeTopic * newTopic) { UnsubscribeTopic ** end = &next; while(*end) { end = &(*end)->next; } *end = newTopic; }
                /** Count the number of topic */
                uint32 count() const { uint32 c = 1; const UnsubscribeTopic * p = next; while (p) { c++; p = p->next; } return c; }

                /** Default constructor */
                UnsubscribeTopic() : next(0) {}
                /** Full constructor */
                UnsubscribeTopic(const DynString & topic)
                    : topic(topic), next(0) {}
                /** Obvious destructor */
                ~UnsubscribeTopic() { if (next) next->suicide(); next = 0; }
            };

            /** A stack based unsubscribe topic */
            struct StackUnsubscribeTopic : public UnsubscribeTopic
            {
                /** Make sure to call the next item as it might be heap allocated */
                void suicide() { if (next) next->suicide(); }

                StackUnsubscribeTopic(const DynString & topic)
                    : UnsubscribeTopic(topic) {}
            };

            
            /** The variable header presence for each possible packet type, the payload presence in each packet type */
            template <ControlPacketType type>
            struct ControlPacketMeta
            {
                /** The fixed header to expect */
                typedef FixedHeaderType<type, 0> FixedHeader;
                /** The variable header to use */
                typedef Properties               VariableHeader;
                /** The payload data if any expected */
                static const bool hasPayload = false;
            };
            
            /** The default fixed field before the variable header's properties */
            template <ControlPacketType type>
            struct FixedField : public Serializable
            {
                /** For action from the packet header to the behaviour */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32) {}
            };

            /** The payload for some packet types. By default, it's empty. */
            template <ControlPacketType type>
            struct Payload : public EmptySerializable
            {
                /** Set the flags marked in the fixed field header */
                inline void setFlags(FixedField<type> &) {}
                /** Set the expected packet size (this is useful for packet whose payload is application defined) */
                inline void setExpectedPacketSize(uint32) {}
            };
            
            /** Declare all the expected control packet type and format */
            template <> struct ControlPacketMeta<CONNECT>       { typedef ConnectHeader          FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = true;  };
            template <> struct ControlPacketMeta<CONNACK>       { typedef ConnectACKHeader       FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; };
            template <> struct ControlPacketMeta<PUBLISH>       { typedef PublishHeader          FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = true;  }; // The packet identifier being dependend upon flags, it's supported in the fixed header type
            template <> struct ControlPacketMeta<PUBACK>        { typedef PublishACKHeader       FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; }; // The packet ID is in the header
            template <> struct ControlPacketMeta<PUBREC>        { typedef PublishReceivedHeader  FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; }; // Same
            template <> struct ControlPacketMeta<PUBREL>        { typedef PublishReleasedHeader  FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; }; // Same
            template <> struct ControlPacketMeta<PUBCOMP>       { typedef PublishCompletedHeader FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; }; // Same
            template <> struct ControlPacketMeta<SUBSCRIBE>     { typedef SubscribeHeader        FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = true;  };
            template <> struct ControlPacketMeta<SUBACK>        { typedef SubscribeACKHeader     FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = true;  };
            template <> struct ControlPacketMeta<UNSUBSCRIBE>   { typedef UnsubscribeHeader      FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = true;  };
            template <> struct ControlPacketMeta<UNSUBACK>      { typedef UnsubscribeACKHeader   FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = true;  };
            template <> struct ControlPacketMeta<PINGREQ>       { typedef PingRequestHeader      FixedHeader; typedef EmptySerializable VariableHeader; static const bool hasPayload = false; };
            template <> struct ControlPacketMeta<PINGRESP>      { typedef PingACKHeader          FixedHeader; typedef EmptySerializable VariableHeader; static const bool hasPayload = false; };
            template <> struct ControlPacketMeta<DISCONNECT>    { typedef DisconnectHeader       FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; };
            template <> struct ControlPacketMeta<AUTH>          { typedef AuthenticationHeader   FixedHeader; typedef Properties        VariableHeader; static const bool hasPayload = false; };


#pragma pack(push, 1)
            /** The fixed field for CONNECT packet */
            template <> struct FixedField<CONNECT> : public MemoryMapped< FixedField<CONNECT> >
            {
                /** The protocol name: "\0\4MQTT" */
                uint8 protocolName[6];
                /** The protocol version */
                uint8 protocolVersion;
                // The connect flags
                union
                {
                   /** This is used to avoid setting all flags by hand */
                   uint8 flags;
                   struct
                   {
#if IsBigEndian == 1
                        /** The user name flag */
                        uint8 usernameFlag : 1;
                        /** The password flag */
                        uint8 passwordFlag : 1;
                        /** The will retain flag */
                        uint8 willRetain : 1;
                        /** The will QoS flag */
                        uint8 willQoS : 2;
                        /** The will flag */
                        uint8 willFlag : 1;
                        /** The clean start session */
                        uint8 cleanStart : 1;
                        /** Reserved bit, should be 0 */
                        uint8 reserved : 1;
#else
                        /** Reserved bit, should be 0 */
                        uint8 reserved : 1;
                        /** The clean start session */
                        uint8 cleanStart : 1;
                        /** The will flag */
                        uint8 willFlag : 1;
                        /** The will QoS flag */
                        uint8 willQoS : 2;
                        /** The will retain flag */
                        uint8 willRetain : 1;
                        /** The password flag */
                        uint8 passwordFlag : 1;
                        /** The user name flag */
                        uint8 usernameFlag : 1;
#endif
                    };
                };
                /** The keep alive time in seconds */
                uint16 keepAlive;
                /** Expected to/from network code */
                void toNetwork() { keepAlive = htons(keepAlive); }
                void fromNetwork() { keepAlive = ntohs(keepAlive); }
                
                /** Check if this header is correct */
                bool check() const { return reserved == 0 && willQoS < 3 && memcmp(protocolName, expectedProtocolName(), sizeof(protocolName)) == 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sCONNECT packet (clean %d, will %d, willQoS %d, willRetain %d, password %d, username %d, keepAlive: %d)\n", (int)indent, "", cleanStart, willFlag, willQoS, willRetain, passwordFlag, usernameFlag, keepAlive); 
                }
#endif

                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) {}
                /** Get the expected protocol name */
                static const uint8 * expectedProtocolName() { static uint8 protocolName[6] = { 0, 4, 'M', 'Q', 'T', 'T' }; return protocolName; }

                /** The default constructor */
                FixedField<CONNECT>() : protocolVersion(5), flags(0), keepAlive(0) { memcpy(protocolName, expectedProtocolName(), sizeof(protocolName));  }
            };
            
            /** The fixed field for the CONNACK packet */
            template <> struct FixedField<CONNACK> : public MemoryMapped< FixedField<CONNACK> >
            {
                /** The acknowledge flag */
                uint8 acknowledgeFlag;
                /** The connect reason */
                uint8 reasonCode;
                /** Expected to/from network code */
                inline void toNetwork() { }
                inline void fromNetwork() { }
                
                /** Check if this header is correct */
                bool check() const { return (acknowledgeFlag & 0xFE) == 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sCONNACK packet (ack %u, reason %u)\n", (int)indent, "", acknowledgeFlag, reasonCode); }
#endif

                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) {}

                /** The default constructor */
                FixedField<CONNACK>() : acknowledgeFlag(0), reasonCode(0) { }
            };

            /** The fixed field for the packet with a packet ID */
            struct FixedFieldWithID : public Serializable
            {
                /** The packet identifier */
                uint16 packetID;
                
                /** Check if this header is correct */
                bool check() const { return true; }
                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) { remLength = length; }

                uint32 getSize() const { return 2; }
                uint32 copyInto(uint8 * buffer) const
                {
                    uint16 p = BigEndian(packetID);
                    memcpy(buffer, &p, sizeof(p));
                    return 2;
                }
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < 2) return NotEnoughData;
                    memcpy(&packetID, buffer, sizeof(packetID)); packetID = BigEndian(packetID);
                    if (remLength == 2) return Shortcut;
                    return 2;
                }

#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sControl packet (id 0x%04X)\n", (int)indent, "", packetID); }
#endif

                
                /** The default constructor */
                FixedFieldWithID() : packetID(0), remLength(2) { }
            private:
                uint32 remLength;
            };

            
            /** The fixed field for the publish acknowledges packets */
            struct FixedFieldWithIDAndReason : public Serializable
            {
                /** The packet identifier */
                uint16 packetID;
                /** The connect reason */
                uint8 reasonCode;
                
                /** Check if this header is correct */
                bool check() const { return true; }
                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) { remLength = length; }

                uint32 getSize() const { return 3; }
                uint32 copyInto(uint8 * buffer) const
                {
                    uint16 p = BigEndian(packetID);
                    memcpy(buffer, &p, sizeof(p));
                    buffer[2] = reasonCode;
                    return 3;
                }
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < 2) return NotEnoughData;
                    memcpy(&packetID, buffer, sizeof(packetID)); packetID = BigEndian(packetID);
                    if (remLength == 2) return Shortcut;
                    reasonCode = buffer[2];
                    if (remLength == 3) return Shortcut; // No need to read properties here
                    return 3;
                }

#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sControl packet (id 0x%04X, reason %u)\n", (int)indent, "", packetID, reasonCode); }
#endif

                
                /** The default constructor */
                FixedFieldWithIDAndReason() : packetID(0), reasonCode(0), remLength(3) { }
            private:
                uint32 remLength;
            };

            /** The SUBSCRIBE header is a generic FixedField header with a packet id */
            template <> struct FixedField<SUBSCRIBE> : public FixedFieldWithID {};
            /** The SUBACK header is a generic FixedField header with a packet id */
            template <> struct FixedField<SUBACK> : public FixedFieldWithID {};
            /** The UNSUBSCRIBE header is a generic FixedField header with a packet id */
            template <> struct FixedField<UNSUBSCRIBE> : public FixedFieldWithID {};
            /** The UNSUBACK header is a generic FixedField header with a packet id */
            template <> struct FixedField<UNSUBACK> : public FixedFieldWithID {};


            /** The PUBACK header is a generic FixedField header with a reason code */
            template <> struct FixedField<PUBACK> : public FixedFieldWithIDAndReason {};
            /** The PUBREC header is a generic FixedField header with a reason code */
            template <> struct FixedField<PUBREC> : public FixedFieldWithIDAndReason {};
            /** The PUBREL header is a generic FixedField header with a reason code */
            template <> struct FixedField<PUBREL> : public FixedFieldWithIDAndReason {};
            /** The PUBCOMP header is a generic FixedField header with a reason code */
            template <> struct FixedField<PUBCOMP> : public FixedFieldWithIDAndReason {};

            /** The fixed field for the DISCONNECT packet which supports shortcut */
            template <> struct FixedField<DISCONNECT>
            {
                /** The connect reason */
                uint8 reasonCode;
                
                /** Check if this header is correct */
                bool check() const { return true; }
                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) { remLength = length; }

                uint32 getSize() const { return 1; }
                uint32 copyInto(uint8 * buffer) const
                {
                    buffer[0] = reasonCode;
                    return 1;
                }
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (remLength == 0) { reasonCode = 0; return Shortcut; }
                    reasonCode = buffer[1];
                    if (remLength == 1) return Shortcut;
                    return 1;
                }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sDISCONNECT packet (reason %u)\n", (int)indent, "", reasonCode); }
#endif
                
                /** The default constructor */
                FixedField<DISCONNECT>() : reasonCode(0), remLength(1) { }
            private:
                uint32 remLength;
            };

            /** The fixed field for the AUTH packet which supports shortcut */
            template <> struct FixedField<AUTH>
            {
                /** The connect reason */
                uint8 reasonCode;
                
                /** Check if this header is correct */
                bool check() const { return true; }
                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 &) {}
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) { remLength = length; }

                uint32 getSize() const { return 1; }
                uint32 copyInto(uint8 * buffer) const
                {
                    buffer[0] = reasonCode;
                    return 1;
                }
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (remLength == 0) { reasonCode = 0; return Shortcut; }
                    reasonCode = buffer[1];
                    return 1;
                }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sAUTH packet (reason %u)\n", (int)indent, "", reasonCode); }
#endif
                
                /** The default constructor */
                FixedField<AUTH>() : reasonCode(0), remLength(1) { }
            private:
                uint32 remLength;
            };



            /** The fixed field for the PUBLISH packet.
                In fact, it's not fixed at all, so we simply implement a serializable interface here */
            template <> struct FixedField<PUBLISH> : public Serializable
            {
                /** The topic name */
                DynString topicName;
                /** The packet identifier */
                uint16 packetID;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return topicName.getSize() + (hasPacketID() ? 2 : 0); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    uint32 o = topicName.copyInto(buffer);
                    if (hasPacketID())
                    {
                        uint16 p = BigEndian(packetID);
                        memcpy(buffer + o, &p, sizeof(p)); o += sizeof(p);
                    }
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    uint32 o = topicName.readFrom(buffer, bufLength);
                    if (isError(o)) return o;
                    buffer += o; bufLength -= o;
                    if (hasPacketID())
                    {
                        if (bufLength < sizeof(packetID)) return NotEnoughData;
                        memcpy(&packetID, buffer, sizeof(packetID)); packetID = BigEndian(packetID);
                        o += sizeof(packetID);
                    }
                    return o;
                }
                /** Check if this header is correct */
                bool check() const { return topicName.check(); }
                /** No action from the packet header to the behaviour here */
                inline void setFlags(const uint8 & u) { flags = &u; }
                /** Some packets are using shortcut length, so we need to know about this */
                inline void setRemainingLength(const uint32 length) {}
                /** Check if the flags says there is a packet identifier */
                inline bool hasPacketID() const { return flags && (*flags & 6) > 0; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sPUBLISH packet (id 0x%04X): ", (int)indent, "", packetID); topicName.dump(out, 0); }
#endif

                /** The default constructor */
                FixedField<PUBLISH>() : flags(0) { }
                
            private:
                /** The main header flags */
                const uint8 * flags;
            };
            
 #pragma pack(pop)
           
            // Payloads
            ///////////////////////////////////////////////////////////////////////////////////////

            /** Helper structure used to store a will message.
                Please notice that only a client code will use this, so the properties are the full blown properties.
                They allocate on heap to store the list of properties */
            struct WillMessage : public Serializable
            {
                /** That's the will properties to attachs to the will message if required */
                Properties          willProperties;
                /** The will topic */
                DynamicString       willTopic;
                /** The last will application message payload */
                DynamicBinaryData   willPayload;

                /** We have a getSize() method that gives the number of bytes requires to serialize this object */
                virtual uint32 getSize() const { return willProperties.getSize() + willTopic.getSize() + willPayload.getSize(); }
                
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's at least getSize() bytes long
                    @return The number of bytes used in the buffer */
                virtual uint32 copyInto(uint8 * buffer) const 
                {
                    uint32 o = willProperties.copyInto(buffer); 
                    o += willTopic.copyInto(buffer+o); 
                    o += willPayload.copyInto(buffer+o);
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer       A pointer to an allocated buffer
                    @param bufLength    The length of the buffer in bytes
                    @return The number of bytes read from the buffer, or a LocalError upon error (use isError() to test for it) */
                virtual uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    uint32 s = willProperties.readFrom(buffer, bufLength), o = 0;
                    if (isError(s)) return s;
                    o += s; buffer += s; bufLength -= s;
                    s = willTopic.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    o += s; buffer += s; bufLength -= s;
                    s = willPayload.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    o += s;
                    return o;
                }
                
                /** Check the will properties validity */
                bool check() const
                {
                    if (!willProperties.check()) return false;
                    PropertyListNode * u = willProperties.head;
                    while (u)
                    {   // Will properties are noted as if their control packet type was 0 (since it's reserved, let's use it)
                        if (!isAllowedProperty((PropertyType)u->property->type, (ControlPacketType)0)) return false;
                        u = u->next;
                    }
                    return willTopic.check() && willPayload.check();
                }

#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sWill message\n", (int)indent, ""); willProperties.dump(out, indent + 2); willTopic.dump(out, indent + 2); willPayload.dump(out, indent + 2); }
#endif


                /** Default construction */
                WillMessage() {}
#if HasCPlusPlus11 == 1
                /** Construction for this message */
                WillMessage(DynamicString && topic, DynamicBinaryData && payload, Properties && properties) :
                    willProperties(properties), willTopic(topic), willPayload(payload) {}
#endif
                /** Copy construction */
                WillMessage(const DynamicString & topic, const DynamicBinaryData & payload, const Properties & properties = Properties()) :
                    willProperties(properties), willTopic(topic), willPayload(payload) {}
            };

            /** The expected payload for connect packet.
                If the MQTTClientOnlyImplementation macro is defined, then this structure is memory mapped (no heap allocation are made).
                This should be safe since, in that case, you'd never receive any such packet from a server */
            template<>
            struct Payload<CONNECT> : public Serializable
            {
                /** This is mandatory to have */
                DynString           clientID;
                /** The Will message, if any provided */
                WillMessage *       willMessage;
                /** The user name that's used for authentication */
                DynString           username;
                /** The password used for authentication */
                DynBinData          password;
                
                /** Set the fixed header */
                inline void setFlags(FixedField<CONNECT> & field) { fixedHeader = &field; }
                /** Set the expected packet size (this is useful for packet whose payload is application defined) */
                inline void setExpectedPacketSize(uint32 sizeInBytes) {}
                
                /** Check if the client ID is valid */
                bool checkClientID() const
                {
                    if (!clientID.length) return true; // A zero length id is allowed
                    // The two lines below are over zealous. We can remove them.
                    static char allowedChars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
                    for (int i = 0; i < clientID.length; i++) if (!isInArray(clientID.data[i], allowedChars)) return false;
                    return true;
                }
                /** Check the will properties validity */
                bool checkWillProperties() const
                {
                    if (fixedHeader && !fixedHeader->willFlag) return true;
                    return willMessage->check();
                }
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return clientID.getSize() + getFilteredSize(); }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    uint32 o = clientID.copyInto(buffer);
                    if (!fixedHeader) return o;
                    if (fixedHeader->willFlag)       o += willMessage->copyInto(buffer+o); 
                    if (fixedHeader->usernameFlag)   o += username.copyInto(buffer+o);
                    if (fixedHeader->passwordFlag)   o += password.copyInto(buffer+o);
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    uint32 s = clientID.readFrom(buffer, bufLength), o = 0;
                    if (isError(s) || !fixedHeader) return s;
                    o += s; buffer += s; bufLength -= s;
                    if (fixedHeader->willFlag)
                    {
                        s = willMessage->readFrom(buffer, bufLength);
                        if (isError(s)) return s;
                        o += s; buffer += s; bufLength -= s;
                    }
                    if (fixedHeader->usernameFlag)
                    {
                        s = username.readFrom(buffer, bufLength);
                        if (isError(s)) return s;
                        o += s; buffer += s; bufLength -= s;
                    }
                    if (fixedHeader->passwordFlag)
                    {
                        s = password.readFrom(buffer, bufLength);
                        if (isError(s)) return s;
                        o += s; buffer += s; bufLength -= s;
                    }
                    return o;
                }
                /** Check if this property is valid */
                bool check() const
                {
                    if (!clientID.check()) return false;
                    if (!fixedHeader) return true;
                    if (fixedHeader->willFlag && !checkWillProperties()) return false;
                    if (fixedHeader->usernameFlag && !username.check()) return false;
                    if (fixedHeader->passwordFlag && !password.check()) return false;
                    return true;
                }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sCONNECT payload\n", (int)indent, ""); 
                    out += MQTTStringPrintf("%*sClientID: ", (int)indent + 2, ""); clientID.dump(out, 0);
                    if (fixedHeader->willFlag) willMessage->dump(out, indent + 2);  // Not testing pointer here, since it's a correctly constructed object is expected
                    out += MQTTStringPrintf("%*sUsername: ", (int)indent + 2, ""); username.dump(out, 0);
                    out += MQTTStringPrintf("%*sPassword: ", (int)indent + 2, ""); password.dump(out, 0);
                }
#endif

                Payload<CONNECT>() : willMessage(0) {}
                
            private:
                /** This is the flags set in the connect header. This is used to ensure good serialization, this is not serialized */
                FixedField<CONNECT> *  fixedHeader;
                uint32 getFilteredSize() const
                {
                    uint32 s = 0;
                    if (!fixedHeader) return s;
                    if (fixedHeader->willFlag)      s += willMessage->getSize();
                    if (fixedHeader->usernameFlag)  s += username.getSize();
                    if (fixedHeader->passwordFlag)  s += password.getSize();
                    return s;
                }
            };

            /** Generic code for payload with plain data */
            template <bool withAllocation>
            struct PayloadWithData : public Serializable
            {
                /** The payload data */
                uint8 * data;
                /** The payload size */
                uint32  size;
                
                /** Set the expected packet size (this is useful for packet whose payload is application defined) */
                inline void setExpectedPacketSize(uint32 sizeInBytes)
                {
                    data = (uint8*)Platform::safeRealloc(data, sizeInBytes);
                    size = data ? sizeInBytes : 0;
                }
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return size; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    memcpy(buffer, data, size);
                    return size;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < size) return NotEnoughData;
                    memcpy(data, buffer, size);
                    return size;
                }
                /** Check if this payload is valid */
                bool check() const { return true; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sPayload (length: %u)\n", (int)indent, "", size); }
#endif                
                PayloadWithData() : data(0), size(0) {}
                ~PayloadWithData() { free0(data); size = 0; }
            };

            template <>
            struct PayloadWithData<false> : public Serializable
            {
                /** The payload data */
                const uint8 * data;
                /** The payload size */
                uint32  size;
                
                /** Set the expected packet size (this is useful for packet whose payload is application defined) */
                inline void setExpectedPacketSize(uint32 sizeInBytes)
                {
                    size = sizeInBytes;
                }
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return size; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    memcpy(buffer, data, size);
                    return size;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < size) return NotEnoughData;
                    data = buffer;
                    return size;
                }
                /** Check if this payload is valid */
                bool check() const { return true; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) { out += MQTTStringPrintf("%*sPayload (length: %u)\n", (int)indent, "", size); }
#endif                
                
                PayloadWithData() : data(0), size(0) {}
                ~PayloadWithData() { data = 0; size = 0; }
            };

            /** The expected payload for subscribe packet */
            template<>
            struct Payload<SUBSCRIBE> : public Serializable
            {
                /** The subscribe topics */
                SubscribeTopic * topics;
                
                /** Set the fixed header */
                inline void setFlags(FixedField<SUBSCRIBE> &) {  }
                /** Set the expected packet size (this is useful for packet whose payload is application defined) */
                inline void setExpectedPacketSize(uint32 sizeInBytes) { expSize = sizeInBytes; }
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return topics ? topics->getSize() : 0; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { return topics ? topics->copyInto(buffer) : 0; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < expSize) return NotEnoughData;
                    if (topics) topics->suicide();
                    topics = new SubscribeTopic();
                    return topics->readFrom(buffer, expSize);
                }
                /** Check if this property is valid */
                bool check() const { return topics ? topics->check() : true; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sSUBSCRIBE Payload\n", (int)indent, ""); 
                    if (topics) topics->dump(out, indent + 2);
                }
#endif                
                
                
                Payload<SUBSCRIBE>() : topics(0), expSize(0) {}
                ~Payload<SUBSCRIBE>() { if (topics) topics->suicide(); topics = 0; }
            private:
                uint32 expSize;
            };

            /** The expected payload for unsubscribe packet */
            template<>
            struct Payload<UNSUBSCRIBE> : public Serializable
            {
                /** The subscribe topics */
                UnsubscribeTopic * topics;
                
                /** Set the fixed header */
                inline void setFlags(FixedField<UNSUBSCRIBE> &) {  }
                /** Set the expected packet size (this is useful for packet whose payload is application defined) */
                inline void setExpectedPacketSize(uint32 sizeInBytes) { expSize = sizeInBytes; }
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return topics ? topics->getSize() : 0; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const { return topics ? topics->copyInto(buffer) : 0; }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < expSize) return NotEnoughData;
                    if (topics) topics->suicide();
                    topics = new UnsubscribeTopic();
                    return topics->readFrom(buffer, expSize);
                }
                /** Check if this property is valid */
                bool check() const { return topics ? topics->check() : true; }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*sUNSUBSCRIBE Payload\n", (int)indent, ""); 
                    if (topics) topics->dump(out, indent + 2);
                }
#endif                
                
                
                Payload<UNSUBSCRIBE>() : topics(0), expSize(0) {}
                ~Payload<UNSUBSCRIBE>() { if (topics) topics->suicide(); topics = 0; }
            private:
                uint32 expSize;
            };

            /** Some packet doesn't have meaningful flags, so skip repeatitive code */ 
            template <ControlPacketType type>
            struct WithoutFlags
            {
                inline void setExpectedPacketSize(uint32 size) {}
                inline void setFlags(FixedField<type> &) { }
            };

            /** The specialization for PUBLISH payload.
                As per the standard, it's opaque data that are application defined */
            template<> struct Payload<PUBLISH> : public PayloadWithData<true>, public WithoutFlags<PUBLISH> {};

            /** The expected payload for a subscribe acknowledge.
                The data is a array of reason code, and it should contains as many reasons as found in subscribe packet received */
            template<> struct Payload<SUBACK> : public PayloadWithData<true>, public WithoutFlags<SUBACK> {};

            /** The expected payload for a unsubscribe acknowledge.
                The data is a array of reason code, and it should contains as many reasons as found in unsubscribe packet received */
            template<> struct Payload<UNSUBACK> : public PayloadWithData<true>, public WithoutFlags<UNSUBACK> {};

            struct ROPayloadPublish  : public PayloadWithData<false> { inline void setFlags(FixedField<PUBLISH> &) { } };
            struct ROPayloadSubACK   : public PayloadWithData<false> { inline void setFlags(FixedField<SUBACK> &) { } };
            struct ROPayloadUnsubACK : public PayloadWithData<false> { inline void setFlags(FixedField<UNSUBACK> &) { } };


            template <ControlPacketType type, bool>
            struct PayloadSelector { typedef Payload<type> PayloadType; };

            template <> struct PayloadSelector<PUBLISH, true>   { typedef ROPayloadPublish  PayloadType; };
            template <> struct PayloadSelector<SUBACK, true>    { typedef ROPayloadSubACK   PayloadType; };
            template <> struct PayloadSelector<UNSUBACK, true>  { typedef ROPayloadUnsubACK PayloadType; };

            // Those don't have any payload, let's simplify them
            template <ControlPacketType type>
            struct EmptySerializableWithoutFlags : public EmptySerializable, public WithoutFlags<type> {};
            // Implementation here
            template <bool a> struct PayloadSelector<CONNACK, a>    { typedef EmptySerializableWithoutFlags<CONNACK>    PayloadType; };
            template <bool a> struct PayloadSelector<PUBACK, a>     { typedef EmptySerializableWithoutFlags<PUBACK>     PayloadType; };
            template <bool a> struct PayloadSelector<PUBREL, a>     { typedef EmptySerializableWithoutFlags<PUBREL>     PayloadType; };
            template <bool a> struct PayloadSelector<PUBREC, a>     { typedef EmptySerializableWithoutFlags<PUBREC>     PayloadType; };
            template <bool a> struct PayloadSelector<PUBCOMP, a>    { typedef EmptySerializableWithoutFlags<PUBCOMP>    PayloadType; };
            template <bool a> struct PayloadSelector<DISCONNECT, a> { typedef EmptySerializableWithoutFlags<DISCONNECT> PayloadType; };
            template <bool a> struct PayloadSelector<AUTH, a>       { typedef EmptySerializableWithoutFlags<AUTH>       PayloadType; };


            /** Generic variable header with heap allocated properties with or without an identifier */
            template <ControlPacketType type, bool propertyMapped>
            struct VHPropertyChooser 
            { 
                typedef typename ControlPacketMeta<type>::VariableHeader VHProperty; 
                typedef Payload<type> PayloadType; 
            };

            /** This is only valid for properties without an identifier */
            template <ControlPacketType type>
            struct VHPropertyChooser<type, true> 
            { 
                typedef PropertiesView VHProperty;
                typedef typename PayloadSelector<type, false>::PayloadType PayloadType;
            };

            /** The base for all control packet */
            struct ControlPacketSerializable : public Serializable
            {
                virtual uint32 computePacketSize(const bool includePayload = true) = 0;
            };

            template <ControlPacketType type, bool propertyMapped = false>
            struct ControlPacket : public ControlPacketSerializable
            {
                /** The fixed header */
                typename ControlPacketMeta<type>::FixedHeader                   header;
                /** The remaining length in bytes, not including the header and itself */
                VBInt                                                           remLength;
                /** The fixed variable header */
                FixedField<type>                                                fixedVariableHeader;
                /** The variable header containing properties */
                typename VHPropertyChooser<type, propertyMapped>::VHProperty    props;
                /** The payload (if any required) */
#if MQTTClientOnlyImplementation == 1
                // Client implementation never need to allocate anything here, either it's client provided or server's buffer provided 
                typename PayloadSelector<type, true>::PayloadType               payload;
#else
                typename PayloadSelector<type, propertyMapped>::PayloadType     payload;
#endif                

#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*s%s control packet (rlength: %u)\n", (int)indent, "", getControlPacketName(type), (uint32)remLength); 
                    header.dump(out, indent + 2);
                    fixedVariableHeader.dump(out, indent + 2);
                    props.dump(out, indent + 2);
                    payload.dump(out, indent + 2);
                }
#endif                

                /** An helper function to actually compute the current packet size, instead of returning the computed value.
                    @param includePayload   If set (default), it compute the payload size and set the remaining length accordingly
                    @return the packet size in bytes if includePayload is true, or the expected size of the payload if not */
                uint32 computePacketSize(const bool includePayload = true)
                {
                    if (includePayload)
                    {
                        uint32 o = fixedVariableHeader.getSize() + props.getSize() + payload.getSize();
                        remLength = o;
                        return o + 1 + remLength.getSize();
                    }
                    return (uint32)remLength - fixedVariableHeader.getSize() - props.getSize();
                }
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return 1 + remLength.getSize() + (uint32)remLength; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    uint32 o = 1; buffer[0] = header.typeAndFlags;
                    o += remLength.copyInto(buffer+o);
                    o += fixedVariableHeader.copyInto(buffer+o);
                    o += props.copyInto(buffer+o);
                    o += payload.copyInto(buffer+o);
                    return o;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < 2) return NotEnoughData;
                    uint32 o = 1; const_cast<uint8&>(header.typeAndFlags) = buffer[0];
                    
                    buffer += o; bufLength -= o;

                    uint32 s = remLength.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    o += s; buffer += s; bufLength -= s;
                    uint32 expLength = (uint32)remLength;
                    if (bufLength < expLength) return NotEnoughData;

                    fixedVariableHeader.setRemainingLength(expLength);
                    s = fixedVariableHeader.readFrom(buffer, bufLength);
                    if (isError(s)) return isShortcut(s) ? o + expLength : s;
                    o += s; buffer += s; bufLength -= s;

                    s = props.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    o += s; buffer += s; bufLength -= s;

                    payload.setExpectedPacketSize(computePacketSize(false));
                    s = payload.readFrom(buffer, bufLength);
                    if (isError(s)) return s;
                    return o + s;
                }
                /** Check if this property is valid */
                bool check() const
                {
                    return header.check() && remLength.check() && fixedVariableHeader.check() && props.checkPropertiesFor(type) && payload.check();
                }
                
                ControlPacket() { payload.setFlags(fixedVariableHeader); fixedVariableHeader.setFlags(header.typeAndFlags); }
            };

            /** Ping control packet are so empty that it makes sense to further optimize their parsing to strict minimum */
            template <ControlPacketType type>
            struct PingTemplate : public ControlPacketSerializable
            {
                /** The fixed header */
                typename ControlPacketMeta<type>::FixedHeader       header;
                
                /** This give the size required for serializing this property header in bytes */
                uint32 getSize() const { return 2; }
                /** The packet size is always 2 */
                uint32 computePacketSize(const bool includePayload = true) { return 2; }
                /** Copy the value into the given buffer.
                    @param buffer   A pointer to an allocated buffer that's getSize() long.
                    @return The number of bytes used in the buffer */
                uint32 copyInto(uint8 * buffer) const
                {
                    buffer[0] = header.typeAndFlags; buffer[1] = 0; return 2;
                }
                /** Read the value from a buffer.
                    @param buffer   A pointer to an allocated buffer that's at least 1 byte long
                    @return The number of bytes read from the buffer, or 0xFF upon error */
                uint32 readFrom(const uint8 * buffer, uint32 bufLength)
                {
                    if (bufLength < 2) return NotEnoughData;
                    const_cast<uint8&>(header.typeAndFlags) = buffer[0];
                    if (buffer[1]) return BadData;
                    return 2;
                }
                /** Check if this property is valid */
                bool check() const { return header.check(); }
#if MQTTDumpCommunication == 1
                void dump(MQTTString & out, const int indent = 0) 
                { 
                    out += MQTTStringPrintf("%*s%s control packet\n", (int)indent, "", getControlPacketName(type)); 
                    header.dump(out, indent + 2);
                }
#endif                

            };
            /** Declare the ping request */
            template<> struct ControlPacket<PINGREQ> : public PingTemplate<PINGREQ> {};
            /** Declare the ping response */
            template<> struct ControlPacket<PINGRESP> : public PingTemplate<PINGRESP> {};

            /** Some useful type definition to avoid understanding the garbage above */
            typedef ControlPacket<PUBLISH>          PublishPacket;
            typedef ControlPacket<PUBLISH, true>    ROPublishPacket;
            typedef ControlPacket<SUBACK>           SubACKPacket;
            typedef ControlPacket<SUBACK, true>     ROSubACKPacket;
            typedef ControlPacket<UNSUBACK>         UnsubACKPacket;
            typedef ControlPacket<UNSUBACK, true>   ROUnsubACKPacket;
            typedef ControlPacket<CONNECT>          ConnectPacket;
            typedef ControlPacket<CONNACK>          ConnACKPacket;
            typedef ControlPacket<CONNACK, true>    ROConnACKPacket;
            typedef ControlPacket<AUTH>             AuthPacket;
            typedef ControlPacket<AUTH, true>       ROAuthPacket;
            typedef ControlPacket<PUBACK>           PubACKPacket;
            typedef ControlPacket<PUBACK, true>     ROPubACKPacket;
            typedef ControlPacket<PUBREC>           PubRecPacket;
            typedef ControlPacket<PUBREC, true>     ROPubRecPacket;
            typedef ControlPacket<PUBREL>           PubRelPacket;
            typedef ControlPacket<PUBREL, true>     ROPubRelPacket;
            typedef ControlPacket<PUBCOMP>          PubCompPacket;
            typedef ControlPacket<PUBCOMP, true>    ROPubCompPacket;
            typedef ControlPacket<DISCONNECT>       DisconnectPacket;
            typedef ControlPacket<DISCONNECT, true> RODisconnectPacket;
            typedef ControlPacket<PINGREQ>          PingReqPacket;
            typedef ControlPacket<PINGRESP>         PingRespPacket;
        }
    }
}



#endif
