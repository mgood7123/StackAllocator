#ifndef SA_HEXDUMP_HPP
#define SA_HEXDUMP_HPP

#include <cctype>
#include <iomanip>
#include <ostream>
#include <functional>

namespace SA {

template <unsigned RowSize, bool ShowAscii, typename T>
struct CustomHexdump
{
    CustomHexdump(const char * indent, const void* data, unsigned length, std::function<void(const T* in, int*outHex, char*outChar)> conv = [](const T * in, int*outHex, char*outChar) { *outHex = (int)*in; *outChar = (char)*in; }) :
    indent(indent), mData(static_cast<const T*>(data)), mLength(length), conv(conv) { }
    const char * indent;
    const T* mData;
    const unsigned mLength;
    const std::function<void(const T* in, int*outHex, char*outChar)> conv;

    class save_cout {
        std::ostream & s;
        std::ios_base::fmtflags f;

        public:

        save_cout() : save_cout(std::cout) {}
        save_cout(std::ostream & o) : s(o), f(o.flags()) {}
        template <typename T2> std::ostream & operator<<(const T2 & item) { return s << item; }
        std::ostream & operator*() { return s; }
        std::ostream * operator->() { return &s; }
        ~save_cout() { s.setf(f); }
    };

};

template <unsigned RowSize, bool ShowAscii, typename T>
std::ostream& operator<<(std::ostream& out_, const CustomHexdump<RowSize, ShowAscii, T>& dump)
{
    typename CustomHexdump<RowSize, ShowAscii, T>::save_cout out(out_);
    *out << std::noshowbase << std::dec;
    out->fill('0');
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
        *out << std::endl;
    }
    return out_;
}

typedef CustomHexdump<16, true, char> Hexdump;

}

#endif // SA_HEXDUMP_HPP