#include "utils/memory/memCache.hh"
#include <chrono> 

namespace ares
{
    MemCache::~MemCache(){
        pollDone = true;
        litQueueTh->join();
        fnQueueTh->join();
        delete litQueueTh;
        delete fnQueueTh;

        varPool.clear();
        constPool.clear();
        nameFnPool.clear();
        nameLitPool.clear();
        
        for( auto&& [name, varptr] : varPool)
            free((void*)varptr.get());
        for( auto&& [name, cnstptr] : constPool)
            free((void*)cnstptr.get());
    }
    cnst_var_sptr MemCache::getVar(ushort n){
        cnst_var_sptr vsptr;
        VarPool::accessor ac;
        if( varPool.insert(ac,n) ) //This thread inserted the key
            ac->second.reset(new Variable(n));
        
        vsptr = ac->second;
        ac.release();
        return vsptr;
    }
    cnst_const_sptr MemCache::getConst(ushort n){
        cnst_const_sptr csptr;
        ConstPool::accessor ac;
        if( constPool.insert(ac,n) )    //This thread inserted the key
            ac->second.reset(new Constant(n));

        csptr = ac->second;
        ac.release();
        return csptr;
    }
    cnst_fn_sptr MemCache::getFn(PoolKey& key){
        NameFnMap::accessor ac;
        nameFnPool.insert(ac,key.name);     //insert it if it doesn't exist
        FnPool& fnPool= ac->second;
        FnPool::accessor fnAc;
        fn_sptr fn;
        if( fnPool.insert(fnAc, key) ){
            //key didn't exist, Create a shared ptr and insert that
            PoolKey* _key = (PoolKey*)&fnAc->first;
            _key->_this = new Function(key.name,key.body);
            fn.reset((Function*)_key->_this,Deleter<const Function>(this));
            fnAc->second = fn;
        }
        else{
            //key  exists but weak ptr might be 0.
            fn = fnAc->second.lock();
            if( not fn ){
                fn.reset( (Function*) fnAc->first._this,Deleter<const Function>(this));
                fnAc->second = fn;
            }
            delete key.body;
        }
        fnAc.release();
        key.body = nullptr;
        return fn;
    }
    cnst_lit_sptr MemCache::getLiteral(PoolKey& key){
        NameLitMap::accessor ac;
        nameLitPool.insert(ac,key.name);     //insert it if it doesn't exist
        LitPool& litPool= ac->second;
        LitPool::accessor litAc;
        lit_sptr lit;
        if( litPool.insert(litAc, key) ){
            //key didn't exist, Create a shared ptr and insert that
            PoolKey* _key = (PoolKey*)&litAc->first;
            _key->_this = new Literal(key.name,key.p,key.body);
            lit.reset((Literal*) _key->_this,Deleter<const Literal>(this));
            litAc->second = lit;
        }
        else{
            //key  exists but weak ptr might be 0.
            lit = litAc->second.lock();
            if( not lit ){
                lit.reset( (Literal*) litAc->first._this,Deleter<const Literal>(this));
                litAc->second = lit;
            }
            delete key.body;
        }
        litAc.release();
        key.body = nullptr;
        return lit;
    }
} // namespace ares
