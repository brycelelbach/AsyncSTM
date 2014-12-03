////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2014 Bryce Adelstein-Lelbach
//  Copyright (c) 2014 Steve Brandt 
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#if !defined(ASTM_DBA88345_57B8_4CC8_A574_D5F007250E94)
#define ASTM_DBA88345_57B8_4CC8_A574_D5F007250E94

#include "astm_config.hpp"

#include <list>
#include <set>
#include <map>
#include <vector>
#include <functional>

namespace astm
{

// transaction should be in a shared_ptr; local_ objects should hold references
// each async {} block needs to hold a clone()'d copy of any shared_var_base variables it uses.

// inside of an async {} block, they can only be read

// IO.then([] { /* write my 100GB of plaintext doubles to file */ });

// if (/* some atomic condition */) then /* start some async thing to make a bitcoin */

struct shared_var_base
{
    virtual ~shared_var_base() {}

    virtual shared_var_base* clone() const = 0;

    virtual void write(shared_var_base const&) = 0;

    virtual ASTM_LOCK lock() const = 0;

    virtual bool operator==(shared_var_base const&) = 0;
};

struct transaction;

template <typename T>
struct shared_var : shared_var_base
{
    struct local_var
    {
      private:
        transaction* trans_;
        shared_var_base* var_;

      public:
        local_var(transaction* trans, shared_var_base* var)
          : trans_(trans)
          , var_(var)
        {}

        T get() const;

        operator T const& () const;

        local_var& operator=(shared_var_base const& rhs);

        local_var& operator=(T const& rhs);
    };

  private:
    T data_;
    mutable ASTM_MUTEX mtx_;

  public:
    shared_var() : data_() {}

    shared_var(T const& t) : data_(t) {}

    shared_var(T&& t) : data_(t) {}

    shared_var(shared_var const& rhs) : data_(rhs.data_) {}

    shared_var& operator=(shared_var const& rhs)
    {
        data_ = rhs.data_; 
    }

    ~shared_var() { }

    // Locks.
    shared_var_base* clone() const
    {
        auto l = lock();
        return new shared_var(data_);
    }

    // Doesn't lock.
    T const& read() const
    {
        return data_;
    }

    // Doesn't lock.
    void write(T const& rhs)
    {
        data_ = rhs;
    }

    // Doesn't lock.
    void write(shared_var_base const& rhs)
    {
        data_ = dynamic_cast<shared_var const*>(&rhs)->read();
    }

    ASTM_LOCK lock() const
    {
        return ASTM_LOCK(mtx_); 
    }

    bool operator==(shared_var_base const& rhs)
    {
        return data_ == dynamic_cast<shared_var const*>(&rhs)->read(); 
    }

    local_var get_local(transaction& trans)
    {
        return local_var(&trans, this); 
    }
};

struct transaction_future
{
    typedef ASTM_FUTURE<void> future_type;

  private:
    transaction* trans_;
    future_type fut_;

  public:
    transaction_future(transaction* trans)
      : trans_(trans)
      , fut_(ASTM_MAKE_READY_FUTURE())
    {}

    transaction_future(transaction& trans)
      : trans_(&trans)
      , fut_(ASTM_MAKE_READY_FUTURE())
    {}

    template <typename F>
    void then(F f);

    void get()
    {
        fut_.get();
    }
};

struct transaction
{
    std::list<
        std::pair<
            // The shared variable we're reading from
            shared_var_base* 
            // The value we read from the variable
          , std::shared_ptr<shared_var_base>
        >
    > read_list;

    std::set<
        shared_var_base* // The shared variable we're writing to
    > write_set;

    std::list<
        std::pair<
            ASTM_FUTURE<void>*    // The future we're writing to
                                  // (if NULL, fire-and-forget semantics)
          , ASTM_FUNCTION<void(transaction*)> // The async action to execute
        >
    > async_list;

    std::map<
        shared_var_base* // The shared variable we're reading from
      , std::shared_ptr<shared_var_base> // Current value of the variable
    > variables;

    void clear()
    {
        read_list.clear();
        write_set.clear();
        async_list.clear();
        variables.clear();
    }

    // This should possibly be called RAII-style from the destructor. 
    bool commit_transaction()
    {
        // Algorithm:
        //
        // 1.) Obtain exclusive access to all the variables. 
        // 2.) Compare our recorded reads against the current values (and fail if needed).
        // 3.) Perform writes, reading from our internal map.
        // 4.) Perform async operations.
        // 5.) Release exclusive access.

        // 1.) Obtain exclusive access to all the variables. 
        std::list<ASTM_LOCK> locks;

        // Variable map is sorted, so order of locking is sorted.
        for ( std::pair<shared_var_base*, std::shared_ptr<shared_var_base> > const& var
            : variables)
        {
            assert(var.first != NULL);

            locks.push_back((*var.first).lock());
        }
        
        // 2.) Compare our recorded reads against the current values (and fail if needed).
        for ( std::pair<shared_var_base*, std::shared_ptr<shared_var_base> > const& var
            : read_list)
        {
            assert(var.first != NULL);

            if (!((*var.first) == (*var.second))) // Perform read operation.
            {
                clear();

                // Transaction fails; exclusive access is released by RAII.
                // Note that this leaves any futures used in a default-constructed
                // state - perhaps we should put an exception (transaction_failed)
                // into them instead. 
                return false;
            }
        }

        // 3.) Perform writes, reading from our internal map.
        for (shared_var_base* var : write_set)
        {
            assert(var != NULL);

            auto it = variables.find(var);

            assert(it != variables.end());

            (*var).write((*(*it).second)); // Perform write operation.
        }

        // 4.) Perform async operations.
        for ( std::pair<ASTM_FUTURE<void>*, ASTM_FUNCTION<void(transaction*)> >& op
            : async_list)
        {
            // If the future is void, use fire-and-forget semantics.
            if (op.first == NULL)
                // For HPX version, just use hpx::apply.
                ASTM_ASYNC(op.second, this);
            else
                (*op.first) = (*op.first).then(std::bind(op.second, this));
        }

        // 5.) Release exclusive access.
        return true; 
    }

    shared_var_base const& read(shared_var_base* var)
    {
        // We have two cases here:
        // * The variable has not been read or written in this transaction, and is
        //   not present in the internal state.
        // * The variable has been read or written previously in this transaction,
        //   and is present in the internal state.

        assert(var != NULL);

        // We will attempt an insertion with a placeholder value, to avoid a
        // speculative read and performing the map lookup twice (find then insert)
        // for first-time entries.
        std::pair<shared_var_base*, std::shared_ptr<shared_var_base> > entry(var, 0);

        // This auto will be a std::pair<iterator, bool>.
        auto result = variables.insert(entry);

        if (result.second == true)
        {
            // FIXME FIXME FIXME: Read list needs a /copy/ of the entry, not a
            // shared_ptr reference to it.

            // Insertion succeeded; this is the first read of the variable.
            (*result.first).second.reset((*var).clone()); // Perform read. 

            // Record the read operation.
            read_list.push_back(*result.first);

            return (*(*result.first).second);
        }
        else
            // Insertion failed; the variable is in the internal state.
            return (*(*result.first).second);
    }

    void write(shared_var_base* var, shared_var_base const& value)
    {
        // We have two cases here:
        // * The variable has not been read or written in this transaction, and is
        //   not present in the internal state.
        // * The variable has been read or written previously in this transaction,
        //   and is present in the internal state.

        assert(var != NULL);

        // We will attempt an insertion, to avoid performing the map lookup twice
        // (find then insert) for first-time entries.
        std::pair<shared_var_base*, std::shared_ptr<shared_var_base> >
            entry(var, std::shared_ptr<shared_var_base>(value.clone()));

        // This auto will be a std::pair<iterator, bool>.
        auto result = variables.insert(entry);

        if (result.second == true)
        {
            // Insertion succeeded; this is the first read of the variable.
            
            // Make sure the variable is in the write set.
            write_set.insert(var); 
        }
        else
        { 
            // FIXME FIXME FIXME: Use write here to avoid the second allocation/
            // creation of a new shared pointer.

            // Insertion failed; the variable is in the internal state.
            (*result.first).second = entry.second; // Perform INTERNAL write.
            
            // Make sure the variable is in the write set.
            write_set.insert(var); 
        }
    }

    // If fut is NULL, then fire-and-forget semantics are used.
    void then(ASTM_FUTURE<void>* fut, ASTM_FUNCTION<void(transaction*)> F)
    {
        std::pair<ASTM_FUTURE<void>*, ASTM_FUNCTION<void(transaction*)> > entry(fut, F);
        async_list.push_back(entry);
    }
};

template <typename T>
shared_var<T>::local_var::operator T const& () const 
{
    return dynamic_cast<shared_var const*>(&trans_->read(var_))->read();
}

template <typename T>
T shared_var<T>::local_var::get() const 
{
    return dynamic_cast<shared_var const*>(&trans_->read(var_))->read();
}

template <typename T>
typename shared_var<T>::local_var&
shared_var<T>::local_var::operator=(shared_var_base const& rhs)
{
    trans_->write(var_, rhs);
    return *this;
}

template <typename T>
typename shared_var<T>::local_var&
shared_var<T>::local_var::operator=(T const& rhs)
{
    shared_var tmp(rhs);
    trans_->write(var_, tmp);
    return *this;
}

template <typename F>
void transaction_future::then(F f)
{
    assert(trans_);
    trans_->then(&fut_, f); 
}

} // namespace astm

#endif // ASTM_DBA88345_57B8_4CC8_A574_D5F007250E94

