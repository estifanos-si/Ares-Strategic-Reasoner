#include "reasoner/prover.hh"
#include "ares.hh"
namespace ares
{ 
    std::atomic<int> Query::nextId = 0;
    AnsIterator Cache::NOT_CACHED(nullptr,-1,nullptr);

    void Prover::compute(Query& query){
        {
            std::lock_guard<std::mutex> lk(lock);
            if( done )return;
            proverCount++;
        }
        Cache* cache = query.cache;
        auto* proverPool = ThreadPoolFactroy::get(1);
        query.pool = nullptr;
        //First stage
        proverPool->post( [&](){compute(query,false);} );
        proverPool->wait();

        //For Starting the subsequent stages
        auto nextstage = [=](Query& q){ proverPool->post( [=](){ compute(q,false); } ); };
        //Check if there are any suspended queries and new answers
        while (cache and cache->hasChanged() and (not query.cb->done))
        {
            //New answers have been obtained, resume suspended queries
            cache->next(nextstage);
            proverPool->wait();
        }

        {
            std::lock_guard<std::mutex> lk(lock);
            proverCount--;
        }
        ThreadPoolFactroy::deallocate(proverPool);
        cv.notify_one();
    }

    void Prover::compute(Query query, const bool isLookup){
        if( query.cb->done or done ) 
            return; //Done stop searching
        
        //Found an answer
        if( Clause::EMPTY_CLAUSE(*query) ){
            (*query.cb)(query->getSubstitution(),query.suffix, isLookup);
            return;
        }
        
        //Instantiate front
        VarSet vset;
        auto* lit = (const Literal*)(*query->front())(query->getSubstitution(),vset);
        query->front() = lit;

        //Check if distinct
        if( lit->get_name() == Namer::DISTINCT ){
            computeDistinct(query);
            return;
        }
        
        //Check if negation
        if( !(*lit) ){
            computeNegation(query);
            return;
        }
        auto* cache = query.cache;
        //Register query as solution or lookup query
        bool shldCache = !contextual(query.context, lit) and cache and !isLookup; 
        //don't want to cache true and does, during negations, and lookups
        auto it = shldCache ? (*cache)[query] : Cache::NOT_CACHED ;    

        if( it.null() ){
            //Solution Node, do an sld-resolution step
            query->pop_front();
            ClauseCB* cb = new ClauseCB(std::move(query));
            compute(lit, query, cb,isLookup);
        }
        else
            //Lookup node, check if there are answers already computed
            lookup(it, query, lit);
    }

    void Prover::lookup(AnsIterator& it, Query& query,const Literal* lit){
        if( query.cb->done or done ) 
            return; //Done stop searching
        /**
         * TODO: CHECK WETHER NOT RENAMING CREATES A PROBLEM
         */
        auto& nxt = it.nxt;
        while (it)
        {
            auto* soln = *it;
            //do an sld-resolution of lit with each solution
            Substitution mgu;
            //FOR DEBUGGING
            // assert( soln->is_ground() );
            if( Unifier::unifyPredicate(*lit, *soln,mgu) ) {
                auto resolvent = std::unique_ptr<Clause>(nxt->clone());
                resolvent->setSubstitution( nxt->getSubstitution() + mgu);
                Query nxtQ(resolvent, query.cb,query.context,query.cache,query.suffix,query.random);
                nxtQ.pool = query.pool;
                if( query.pool ) //We are not trying to prove a negation
                    query.pool->post([=] { compute(nxtQ,true);});
                else    //We are in the middle of proving a negation
                    compute(nxtQ,true);
            }
            ++it;
        }
    }
    
    void Prover::compute(const Literal* lit,Query q, CallBack* cb,const bool lookup){
        if( cb->done or done ) 
            return; //Done stop searching
        bool isContxt = contextual(q.context, lit);
        //If its contextual or lit has was head of the resolvent b/n a lookup node and a solution we dont cache
        auto cblit = new LiteralCB(lit, std::unique_ptr<CallBack>(cb), ( (isContxt or lookup) ? nullptr : q.cache) );
        std::shared_ptr<CallBack> cbsptr(cblit);
        //Resolve against all program clauses whose heads unify with lit.
        //Need to know weather lit is contextual or not, so as to determine the correct knowldge base to check
        const KnowledgeBase& kb_t = isContxt ? *q.context : *this->kb;
        SuffixRenamer renamer(q.suffix);
        //Resolve against each candidate
        const auto& cs = kb_t[lit->get_name()]->getElements();
        UIterator<const ares::Clause *>* it = q.random ? new RandIterator(cs) : new UIterator(cs);
        while (*it)
        {
            std::unique_ptr<Clause> gn( resolve( lit, ***it, renamer));
            if( gn ) 
            {
                Query qn(gn, cbsptr, q.context,q.cache,renamer.gets(),q.random);
                qn.pool = q.pool;
                if( q.pool ) //We are not trying to prove a negation
                    q.pool->post([=]{ compute(qn, false);});
                else //We are in the middle of proving a negation
                    compute(qn, false);
            }
            ++(*it);
        }
        delete it;
    }

    Clause* Prover::resolve(const Literal* lit, const Clause& c,SuffixRenamer& vr){
        auto* mgu = new Substitution();
        VarSet vset;
        auto renamed = (const Literal*)(*c.getHead())(vr, vset);
        if ( !Unifier::unifyPredicate(*lit, *renamed,*mgu) ){
            delete mgu;
            return nullptr;
        }
        ClauseBody* renamedBody = new ClauseBody(c.size());
        c.renameBody(*renamedBody,vr);
        return new Clause( nullptr, renamedBody,mgu);
    }
    void Prover::computeNegation(Query& query){
        //front has the form (not A1)
        auto& front = query->front();
        if( not front->is_ground() ){
            query->delayFront();
            compute(query, false);
            return;
        }
        
        //Get A1 from memCache
        PoolKey key{front->get_name(), new Body(front->getBody().begin(), front->getBody().end()), true};
        const Literal* pstvLit = Ares::memCache->getLiteral(key);

        //Prove <-A1
        //Callback which stops all computations when one answer is computed
        std::atomic_bool done = false;
        std::unique_ptr<Clause> g(nullptr);
        Query q(g,query.cb,query.context,nullptr,0,query.random);//pstvLit is ground so we could restart the suffix
        compute(pstvLit, q, new ClauseCBOne(done,nullptr));
        if( done ) //Atleast one answer has been found, so <-A1 succedeed 
            return;

        //Failed to prove <-A1, therefore P |= <-(not)A1
        query->pop_front();
        //Continue proving <-A2,..,An
        compute(query, false);
    }
    void Prover::computeDistinct(Query& query){
        auto& front = query->front();
        if( not front->is_ground() ){
            query->delayFront();
            compute(query, false);
            return;
        }

        if( *front->getArg(0) == (*front->getArg(1)) )
            return;
        
        query->pop_front();
        compute(query, false);
    }
} // namespace ares
