#pragma once

#include <string>
#include <iostream>
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
}
