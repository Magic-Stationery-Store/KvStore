#include<fstream>
#include<string>
#include<iostream>

int main()
{
    std::cout << "Choose write or read: ";
    std::string command;
    if(!(std::cin >> command))
    {
        return 1;
    }

    if(command == "write")
    {
        std::ofstream output("demo.txt",std::ios::app);
        if(!output.is_open())
        {
            std::cerr << "Failed to open file for writing\n";
            return 1;
        }
        output << "name alice\n";
        output << "age 20\n";

        output.close();

        if(!output)
        {
            std::cerr << "Failed to write to file\n";
            return 1;
        }
        
        std::cout << "Write successful\n";
    }
    else if(command == "read")
    {
        std::ifstream input("demo.txt");

        if(!input.is_open())
        {
            std::cerr << "Failed to open file for reading\n";
            return 1;
        }
        std::string line;
        while(std::getline(input,line))
        {
            std::cout << line << "\n";
        }
        if (input.bad() || (input.fail() && !input.eof()))
        {
            std::cerr << "Failed to read from file\n";
            return 1;
        }
        

    }
    else
    {
        std::cerr << "Unknown command: " << command << "\n";
        return 1;
    }
   return 0;

}