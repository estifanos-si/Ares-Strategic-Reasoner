#ifndef TERM_HH
#define TERM_HH

#include <string>
#include <vector>
#include "reasoner/substitution.hh"

namespace Ares
{
    //Variables, functions, constants all inherit from this abstract class.
    class Term
    {
    protected:
        std::string name;

    public:
        Term(std::string n):name(n){}
        /**
         * Use Term.operator()(Substitution sub) to create a deep clone.
         * Protect against accidental copying,assignment, and return by value.
        */
        Term(const Term &t) = delete;
        Term& operator = (const Term &t) = delete;
        /**
         * Apply Substitution sub on this term.
         * if inplace=false, then no new term is created the substitution 
         * is done inplace.
         * else a new term is created and that is modified using sub.
         */
        virtual Term* operator ()(Substitution &sub,bool inPlace=false);
        virtual bool isGround();
        std::string getName() const {return name;}
    };  
} // namespace Ares

#endif