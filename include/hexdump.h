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
    indent(indent), mData(data), mLength(length), conv(conv) { }
    const char * indent;
    const T* mData;
    const unsigned mLength;
    const std::function<void(const T* in, int*outHex, char*outChar)> conv;
};

template <unsigned RowSize, bool ShowAscii, typename T>
std::ostream& operator<<(std::ostream& out, const CustomHexdump<RowSize, ShowAscii, T>& dump)
{
    out.fill('0');
    for (int i = 0; i < dump.mLength; i += RowSize)
    {
        out << dump.indent << "0x" << std::setw(6) << std::hex << i << ": ";
        for (int j = 0; j < RowSize; ++j)
        {
            if (i + j < dump.mLength)
            {
                int h;
                char c;
                dump.conv(std::addressof(dump.mData[i + j]), &h, &c);
                out << std::hex << std::setw(2) << h << " ";
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
    }
    return out;
}

typedef CustomHexdump<16, true, char> Hexdump;

#endif // HEXDUMP_HPP