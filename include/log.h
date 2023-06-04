#pragma once

#include <string>
#include <iostream>
#include <sstream>

namespace SA {
    extern void Logr();
    extern void Loga();
    extern void Logib();
    extern void Logwb();
    extern void Logeb();
    
    template <typename T>
    void Logi(const T & s) {
        Logib();
        std::cout << s;
        Logr();
        std::cout << std::endl;
    }
    
    template <typename T>
    void Logw(const T & s) {
        Logwb();
        std::cout << s;
        Logr();
        std::cout << std::endl;
    }
    
    template <typename T>
    void Loge(const T & s) {
        Logeb();
        std::cout << s;
        Logr();
        std::cout << std::endl;
    }
    
    template <typename T>
    void Loga(const T & s) {
        Logeb();
        std::cout << s;
        std::cout << std::endl;
        Loga();
    }


    template <typename T>
    void Logi(std::ostream & out, const T & s) {
        Logib();
        out << s;
        Logr();
        out << std::endl;
    }
    
    template <typename T>
    void Logw(std::ostream & out, const T & s) {
        Logwb();
        out << s;
        Logr();
        out << std::endl;
    }
    
    template <typename T>
    void Loge(std::ostream & out, const T & s) {
        Logeb();
        out << s;
        Logr();
        out << std::endl;
    }
    
    template <typename T>
    void Loga(std::ostream & out, const T & s) {
        Logeb();
        out << s;
        out << std::endl;
        Loga();
    }
}
