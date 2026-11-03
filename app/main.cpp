#include<iostream>
#include<exception>
#include"kv_store.h"
#include<string>
#include<sstream>
int main()
{
    try{
    KVStore store("kvstore.log");
    std::cout << "Commands:\n"
    << "Put <key> value\n"
    << "Get <key>\n"
    << "Delete <key>\n"
    << "Sync\n"
    << "Flush\n"
    << "Compact\n"
    << "Exit\n";
    std::string line;
    while(std::cout << "> " && std::getline(std::cin, line))
    {
        std::istringstream input(line);
        std::string command;
        input >> command;
        if(command.empty())
        {
            continue;
        }
        if(command == "Put")
        {
            std::string key, value, extra;
            if (!(input >> key >> value) || (input >> extra))
            {
                std::cout << "Usage: Put <key> <value>\n";
                continue;
            }
            store.put(key, value);
            std::cout << "OK\n";
        }
       else if(command == "Get")
       {
            std::string key, extra;
            if (!(input >> key) || (input >> extra))
            {
                std::cout << "Usage: Get <key>\n";
                continue;
            }
            auto value = store.get(key);
            if(value.has_value())
            {
                std::cout << *value << "\n";
            }
            else
            {
                std::cout << "Key not found\n";
            }
        }
        else if(command == "Delete")
        {
            std::string key, extra;
            if (!(input >> key) || (input >> extra))
            {
                std::cout << "Usage: Delete <key>\n";
                continue;
            }
           std::cout << (store.remove(key) ? "OK\n" : "Key not found\n");
        }
        else if(command == "Sync")
        {
            std::string extra;
            if(input >> extra)
            {
                std::cout << "Usage: Sync\n";
                continue;
            }
            store.sync();
            std::cout << "SYNCED\n";
        }
        else if(command == "Flush")
        {
            std::string extra;
            if(input >> extra)
            {
                std::cout << "Usage: Flush\n";
                continue;
            }
            store.flush();
            std::cout << "FLUSHED\n";
        }
        else if(command == "Compact")
        {
            std::string extra;
            if(input >> extra)
            {
                std::cout << "Usage: Compact\n";
                continue;
            }
            store.compact();
            std::cout << "COMPACTED\n";
        }
        else if(command == "Exit")
        {
            break;
        }
        else
        {
            std::cout << "Unknown command\n";
        }
    }
}
    catch(const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
