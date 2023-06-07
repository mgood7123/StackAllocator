#ifndef HEXDUMP_HPP
#define HEXDUMP_HPP

#include <cctype>
#include <iomanip>
#include <ostream>
#include <functional>

template <unsigned RowSize, bool ShowAscii, typename T>
struct CustomHexdump
{
    CustomHexdump(const char * indent, const T* data, unsigned length, std::function<void(const T* in, int*outHex, char*outChar)> conv = [](const T * in, int*outHex, char*outChar) { *outHex = (int)*in; *outChar = (char)*in; }) :
    CustomHexdump("CustomHexDump (NO TAG HAS BEEN SET)", indent, data, length, conv) { }
    CustomHexdump(const char * tag, const char * indent, const T* data, unsigned length, std::function<void(const T* in, int*outHex, char*outChar)> conv = [](const T * in, int*outHex, char*outChar) { *outHex = (int)*in; *outChar = (char)*in; }) :
    tag(tag), indent(indent), mData(data), mLength(length), conv(conv) { }
    const char * tag;
    const char * indent;
    const T* mData;
    const unsigned mLength;
    const std::function<void(const T* in, int*outHex, char*outChar)> conv;
};

#define HEXDUMP__VAL(in, out, len) char out[len+1]; out[len] = 0; snprintf(out, len+1, "%0*X", len, in);

template <unsigned RowSize, bool ShowAscii, typename T>
std::ostream& operator<<(std::ostream& out, const CustomHexdump<RowSize, ShowAscii, T>& dump)
{
    for (int i = 0; i < dump.mLength; i += RowSize)
    {
        HEXDUMP__VAL(i, i_h, 6);
        out << "[ " << dump.tag << " ] " << dump.indent << "0x" << i_h << ": ";
        for (int j = 0; j < RowSize; ++j)
        {
            if (i + j < dump.mLength)
            {
                int h;
                char c;
                dump.conv(std::addressof(dump.mData[i + j]), &h, &c);
                HEXDUMP__VAL(h, h_h, 2);
                out << h_h << " ";
            }
            else
            {
                out << "   ";
            }
        }
        
        out << " ";
        if (ShowAscii)
        {
            for (int j = 0; j < RowSize; ++j)
            {
                if (i + j < dump.mLength)
                {
                    int h;
                    char c;
                    dump.conv(std::addressof(dump.mData[i + j]), &h, &c);
                    if (std::isprint(c))
                    {
                        out << c;
                    }
                    else
                    {
                        out << ".";
                    }
                }
            }
        }
        out << std::endl;
        out << std::dec << std::setw(0);
    }
    return out;
}

typedef CustomHexdump<16, true, char> Hexdump;

#endif // HEXDUMP_HPP