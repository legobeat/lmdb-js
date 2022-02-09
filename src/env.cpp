#include "lmdb-js.h"
using namespace napi;
using namespace node;

#define IGNORE_NOTFOUND    (1)
//thread_local Nan::Persistent<Function>* EnvWrap::txnCtor;
//thread_local Nan::Persistent<Function>* EnvWrap::dbiCtor;

pthread_mutex_t* EnvWrap::envsLock = EnvWrap::initMutex();
std::vector<env_path_t> EnvWrap::envs;

pthread_mutex_t* EnvWrap::initMutex() {
    pthread_mutex_t* mutex = new pthread_mutex_t;
    pthread_mutex_init(mutex, nullptr);
    return mutex;
}

EnvWrap::EnvWrap(const CallbackInfo& info) {
    Env napiEnv = info.Env();
    int rc;
    rc = mdb_env_create(&(this->env));

    if (rc != 0) {
        mdb_env_close(this->env);
        return throwLmdbError(info.Env(), rc);
    }

    this->env = nullptr;
    this->currentWriteTxn = nullptr;
	this->currentReadTxn = nullptr;
    this->writeTxn = nullptr;
    this->writeWorker = nullptr;
	this->readTxnRenewed = false;
    this->writingLock = new pthread_mutex_t;
    this->writingCond = new pthread_cond_t;
    pthread_mutex_init(this->writingLock, nullptr);
    pthread_cond_init(this->writingCond, nullptr);
}

EnvWrap::~EnvWrap() {
    // Close if not closed already
    closeEnv();
    if (this->compression)
        this->compression->Unref();
    pthread_mutex_destroy(this->writingLock);
    pthread_cond_destroy(this->writingCond);
    
}

void EnvWrap::cleanupStrayTxns() {
    if (this->currentWriteTxn) {
        mdb_txn_abort(this->currentWriteTxn->txn);
        this->currentWriteTxn->removeFromEnvWrap();
    }
    while (this->readTxns.size()) {
        TxnWrap *tw = *this->readTxns.begin();
        mdb_txn_abort(tw->txn);
        tw->removeFromEnvWrap();
    }
}

template<class T>
int applyUint32Setting(int (*f)(MDB_env *, T), MDB_env* e, Local<Object> options, T dflt, const char* keyName) {
    int rc;
    const Local<Value> value = options->Get(Nan::GetCurrentContext(), String::New(env, keyName)).ToLocalChecked();
    if (value->IsUint32()) {
        rc = f(e, value->Uint32Value(Nan::GetCurrentContext()).FromJust());
    }
    else {
        rc = f(e, dflt);
    }

    return rc;
}

class SyncWorker : public AsyncWorker {
  public:
    SyncWorker(MDB_env* env, Function *callback)
      : AsyncWorker(callback), env(env) {}

    void Execute() {
        int rc = mdb_env_sync(env, 1);
        if (rc != 0) {
            SetErrorMessage(mdb_strerror(rc));
        }
    }

    void OnOK() {
        Local<v8::Value> argv[] = {
            Nan::Null()
        };

        callback->Call(1, argv, async_resource);
    }

  private:
    MDB_env* env;
};

class CopyWorker : public AsyncWorker {
  public:
    CopyWorker(MDB_env* env, char* inPath, int flags, Function *callback)
      : AsyncWorker(callback), env(env), path(strdup(inPath)), flags(flags) {
      }
    ~CopyWorker() {
        free(path);
    }

    void Execute() {
        int rc = mdb_env_copy2(env, path, flags);
        if (rc != 0) {
            fprintf(stderr, "Error on copy code: %u\n", rc);
            SetErrorMessage("Error on copy");
        }
    }

    void OnOK() {
        Local<v8::Value> argv[] = {
            Nan::Null()
        };

        callback->Call(1, argv, async_resource);
    }

  private:
    MDB_env* env;
    char* path;
    int flags;
};
MDB_txn* EnvWrap::getReadTxn() {
    MDB_txn* txn = writeTxn ? writeTxn->txn : nullptr;
    if (txn)
        return txn;
    txn = currentReadTxn;
    if (readTxnRenewed)
        return txn;
    if (txn)
        mdb_txn_renew(txn);
    else {
        throwError("No current read transaction available");
        return nullptr;
    }
    readTxnRenewed = true;
    return txn;
}

#ifdef MDB_RPAGE_CACHE
static int encfunc(const MDB_val* src, MDB_val* dst, const MDB_val* key, int encdec)
{
    chacha8(src->mv_data, src->mv_size, (uint8_t*) key[0].mv_data, (uint8_t*) key[1].mv_data, (char*)dst->mv_data);
    return 0;
}
#endif

void cleanup(void* data) {
    ((EnvWrap*) data)->closeEnv();
}

napi_value EnvWrap::open(const CallbackInfo& info) {
    int rc;

    // Get the wrapper
    if (!this->env) {
        return throwError("The environment is already closed.");
    }
    Local<Object> options = Local<Object>::Cast(info[0]);
    Local<Number> flagsValue = Local<Number>::Cast(info[1]);
    int flags = flagsValue->IntegerValue(Nan::GetCurrentContext()).FromJust();
    Local<Number> jsFlagsValue = Local<Number>::Cast(info[2]);
    int jsFlags = jsFlagsValue->IntegerValue(Nan::GetCurrentContext()).FromJust();

    Compression* compression = nullptr;
    Local<Value> compressionOption = options->Get(Nan::GetCurrentContext(), String::New(env, "compression")).ToLocalChecked();
    if (compressionOption->IsObject()) {
        compression = this->compression = Nan::ObjectWrap::Unwrap<Compression>(Nan::To<Object>(compressionOption).ToLocalChecked());
        this->compression->Ref();
    }
    char* keyBuffer;
    Local<Value> keyBytesValue = options->Get(Nan::GetCurrentContext(), String::New(env, "keyBytes")).ToLocalChecked();
    if (!keyBytesValue->IsArrayBufferView())
        fprintf(stderr, "Invalid key buffer\n");
    keyBuffer = node::Buffer::Data(keyBytesValue);
    setFlagFromValue(&jsFlags, SEPARATE_FLUSHED, "separateFlushed", false, options);
    Local<String> path = Local<String>::Cast(options->Get(Nan::GetCurrentContext(), String::New(env, "path")).ToLocalChecked());
    int pathLength = path->Length();
    uint8_t* pathBytes = new uint8_t[pathLength + 1];
    int bytes = path->WriteOneByte(Isolate::GetCurrent(), pathBytes, 0, pathLength + 1, v8::String::WriteOptions::NO_OPTIONS);
    if (bytes != pathLength)
        fprintf(stderr, "Bytes do not match %u %u", bytes, pathLength);
    if (pathBytes[bytes])
        fprintf(stderr, "String is not null-terminated");
    // Parse the maxDbs option
    int maxDbs = 12;
    Local<Value> option = options->Get(Nan::GetCurrentContext(), String::New(env, "maxDbs")).ToLocalChecked();
    if (option->IsNumber())
        maxDbs = option->IntegerValue(Nan::GetCurrentContext()).FromJust();


    mdb_size_t mapSize = 0;
    // Parse the mapSize option
    option = options->Get(Nan::GetCurrentContext(), String::New(env, "mapSize")).ToLocalChecked();
    if (option->IsNumber())
        mapSize = option->IntegerValue(Nan::GetCurrentContext()).FromJust();
    int pageSize = 8192;
    // Parse the mapSize option
    option = options->Get(Nan::GetCurrentContext(), String::New(env, "pageSize")).ToLocalChecked();
    if (option->IsNumber())
        pageSize = option->IntegerValue(Nan::GetCurrentContext()).FromJust();
    int maxReaders = 126;
    // Parse the mapSize option
    option = options->Get(Nan::GetCurrentContext(), String::New(env, "maxReaders")).ToLocalChecked();
    if (option->IsNumber())
        maxReaders = option->IntegerValue(Nan::GetCurrentContext()).FromJust();

    uint8_t* encryptKey = nullptr;
    Local<Value> encryptionKey = options->Get(Nan::GetCurrentContext(), String::New(env, "encryptionKey")).ToLocalChecked();
    if (!encryptionKey->IsUndefined()) {
        unsigned int l = Local<String>::Cast(encryptionKey)->Length();
        encryptKey = new uint8_t[l];
        int utfWritten = 0;
        Local<String>::Cast(encryptionKey)->WriteUtf8(Isolate::GetCurrent(),
            (char*) encryptKey, l, &utfWritten, v8::String::WriteOptions::NO_NULL_TERMINATION);
        if (utfWritten != 32) {
            return throwError("Encryption key must be 32 bytes long");
        }
        #ifndef MDB_RPAGE_CACHE
        return throwError("Encryption not supported with data format version 1");
        #endif
    }

    rc = this->openEnv(flags, jsFlags, (const char*)pathBytes, keyBuffer, compression, maxDbs, maxReaders, mapSize, pageSize, (char*)encryptKey);
    delete[] pathBytes;
    if (rc < 0)
        return throwLmdbError(info.Env(), rc);
    node::AddEnvironmentCleanupHook(Isolate::GetCurrent(), cleanup, ew);
    return info.GetReturnValue().Set(Nan::New<Number>(rc));
}
int EnvWrap::openEnv(int flags, int jsFlags, const char* path, char* keyBuffer, Compression* compression, int maxDbs,
        int maxReaders, mdb_size_t mapSize, int pageSize, char* encryptionKey) {
    pthread_mutex_lock(envsLock);
    this->keyBuffer = keyBuffer;
    this->compression = compression;
    this->jsFlags = jsFlags;
    MDB_env* env = this->env;
    for (auto envPath = envs.begin(); envPath != envs.end();) {
        char* existingPath = envPath->path;
        if (!strcmp(existingPath, path)) {
            envPath->count++;
            mdb_env_close(env);
            this->env = envPath->env;
            pthread_mutex_unlock(envsLock);
            return 0;
        }
        ++envPath;
    }
    int rc;
    rc = mdb_env_set_maxdbs(env, maxDbs);
    if (rc) goto fail;
    rc = mdb_env_set_maxreaders(env, maxReaders);
    if (rc) goto fail;
    rc = mdb_env_set_mapsize(env, mapSize);
    if (rc) goto fail;
    #ifdef MDB_RPAGE_CACHE
    rc = mdb_env_set_pagesize(env, pageSize);
    if (rc) goto fail;
    #endif
    if ((size_t) encryptionKey > 100) {
        MDB_val enckey;
        enckey.mv_data = encryptionKey;
        enckey.mv_size = 32;
        #ifdef MDB_RPAGE_CACHE
        rc = mdb_env_set_encrypt(this->env, encfunc, &enckey, 0);
        #else
        rc = -1;
        #endif
        if (rc != 0) goto fail;
    }

    if (flags & MDB_OVERLAPPINGSYNC) {
        flags |= MDB_PREVSNAPSHOT;
    }

    if (flags & MDB_NOLOCK) {
        fprintf(stderr, "You chose to use MDB_NOLOCK which is not officially supported by node-lmdb. You have been warned!\n");
    }

    // Set MDB_NOTLS to enable multiple read-only transactions on the same thread (in this case, the nodejs main thread)
    flags |= MDB_NOTLS;
    // TODO: make file attributes configurable
    // *String::Utf8Value(Isolate::GetCurrent(), path)
    rc = mdb_env_open(env, path, flags, 0664);
    mdb_env_get_flags(env, (unsigned int*) &flags);

    if (rc != 0) {
        mdb_env_close(env);
        goto fail;
    }
    env_path_t envPath;
    envPath.path = strdup(path);
    envPath.env = env;
    envPath.count = 1;
    envs.push_back(envPath);
    pthread_mutex_unlock(envsLock);
    return 0;

    fail:
    pthread_mutex_unlock(envsLock);
    this->env = nullptr;
    return rc;
}
napi_value EnvWrap::getMaxKeySize(const CallbackInfo& info) {
    return Number::New(mdb_env_get_maxkeysize(this->env));
}


#ifdef _WIN32
// TODO: I think we should switch to DeleteFileW (but have to convert to UTF16)
#define unlink DeleteFileA
#else
#include <unistd.h>
#endif

void EnvWrap::closeEnv() {
    if (!env)
        return;
    node::RemoveEnvironmentCleanupHook(Isolate::GetCurrent(), cleanup, this);
    cleanupStrayTxns();
    pthread_mutex_lock(envsLock);
    for (auto envPath = envs.begin(); envPath != envs.end(); ) {
        if (envPath->env == env) {
            envPath->count--;
            unsigned int envFlags; // This is primarily useful for detecting termination of threads and sync'ing on their termination
            mdb_env_get_flags(env, &envFlags);
            if (envFlags & MDB_OVERLAPPINGSYNC)
                mdb_env_sync(env, 1);
            if (envPath->count <= 0) {
                // last thread using it, we can really close it now
                mdb_env_close(env);
                if (jsFlags & DELETE_ON_CLOSE) {
                    unlink(envPath->path);
                    //unlink(strcat(envPath->path, "-lock"));
                }
                envs.erase(envPath);
            }
            break;
        }
        ++envPath;
    }
    pthread_mutex_unlock(envsLock);

    env = nullptr;
}
extern "C" EXTERN void closeEnv(double ewPointer) {
    EnvWrap* ew = (EnvWrap*) (size_t) ewPointer;
    this->closeEnv();
}

napi_value EnvWrap::close(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }
    this->closeEnv();
}

napi_value EnvWrap::stat(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }
     int rc;
    MDB_stat stat;

    rc = mdb_env_stat(this->env, &stat);
    if (rc != 0) {
        return throwLmdbError(info.Env(), rc);
    }
    Object stats = Object::New(info.Env());
    stats.Set("pageSize", Number::New(info.Env(), stat.ms_psize));
    stats.Set("treeDepth", Number::New(info.Env(), stat.ms_depth));
    stats.Set("treeBranchPageCount", Number::New(info.Env(), stat.ms_branch_pages));
    stats.Set("treeLeafPageCount", Number::New(info.Env(), stat.ms_leaf_pages));
    stats.Set("entryCount", Number::New(info.Env(), stat.ms_entries));
    stats.Set("overflowPages", Number::New(info.Env(), stat.ms_overflow_pages));
    return stats;
}

napi_value EnvWrap::freeStat(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }
    int rc;
    MDB_stat stat;

    rc = mdb_stat(txn->txn, 0, &stat);
    if (rc != 0) {
        return throwLmdbError(info.Env(), rc);
    }
    Object stats = Object::New(info.Env());
    stats.Set("pageSize", Number::New(info.Env(), stat.ms_psize));
    stats.Set("treeDepth", Number::New(info.Env(), stat.ms_depth));
    stats.Set("treeBranchPageCount", Number::New(info.Env(), stat.ms_branch_pages));
    stats.Set("treeLeafPageCount", Number::New(info.Env(), stat.ms_leaf_pages));
    stats.Set("entryCount", Number::New(info.Env(), stat.ms_entries));
    stats.Set("overflowPages", Number::New(info.Env(), stat.ms_overflow_pages));
    return stats;
}

napi_value EnvWrap::info(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }
    int rc;
    MDB_envinfo envinfo;

    rc = mdb_env_info(this->env, &envinfo);
    if (rc != 0) {
        return throwLmdbError(info.Env(), rc);
    }
    Object stats = Object::New(info.Env());
    stats.Set("mapAddress", Number::New(info.Env(), envinfo.me_mapaddr));
    stats.Set("mapSize", Number::New(info.Env(), envinfo.me_mapsize));
    stats.Set("lastPageNumber", Number::New(info.Env(), envinfo.me_last_txnid));
    stats.Set("lastTxnId", Number::New(info.Env(), envinfo.ms_leaf_pages));
    stats.Set("maxReaders", Number::New(info.Env(), envinfo.me_maxreaders));
    stats.Set("numReaders", Number::New(info.Env(), envinfo.me_numreaders));
    return stats;
}

napi_value EnvWrap::readerCheck(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }

    int rc, dead;
    rc = mdb_reader_check(this->env, &dead);
    if (rc != 0) {
        return throwLmdbError(info.Env(), rc);
    }
    return Number::New(dead);
}

napi_array readerStrings;
MDB_msg_func* printReaders = ([](const char* message, void* ctx) -> int {
    readerStrings->Set(Nan::GetCurrentContext(), readerStrings->Length(), String::New(env, message)).ToChecked();
    return 0;
});

napi_value EnvWrap::readerList(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }

    napi_create_array(info.Env(), &readerStrings);
    int rc;
    rc = mdb_reader_list(this->env, printReaders, nullptr);
    if (rc != 0) {
        return throwLmdbError(info.Env(), rc);
    }
    return readerStrings;
}


napi_value EnvWrap::copy(const CallbackInfo& info) {
    if (!this->env) {
        return throwError("The environment is already closed.");
    }

    // Check that the correct number/type of arguments was given.
    if (!info[0]->IsString()) {
        return throwError("Call env.copy(path, compact?, callback) with a file path.");
    }
    if (!info[info.Length() - 1]->IsFunction()) {
        return throwError("Call env.copy(path, compact?, callback) with a file path.");
    }
    Nan::Utf8String path(info[0].As<String>());

    int flags = 0;
    if (info.Length() > 1 && info[1]->IsTrue()) {
        flags = MDB_CP_COMPACT;
    }

    Function* callback = new Function(
      Local<v8::Function>::Cast(info[info.Length()  > 2 ? 2 : 1])
    );

    CopyWorker* worker = new CopyWorker(
      this->env, *path, flags, callback
    );

    AsyncQueueWorker(worker);
}

napi_value EnvWrap::detachBuffer(const CallbackInfo& info) {

    #if NODE_VERSION_AT_LEAST(12,0,0)
    Local<v8::ArrayBuffer>::Cast(info[0])->Detach();
    #endif
}

napi_value EnvWrap::beginTxn(const CallbackInfo& info) {


    Nan::MaybeLocal<Object> maybeInstance;

    int flags = info[0]->IntegerValue(Nan::GetCurrentContext()).FromJust();
    if (!(flags & MDB_RDONLY)) {
        EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
        MDB_env *env = this->env;
        unsigned int envFlags;
        mdb_env_get_flags(env, &envFlags);
        MDB_txn *txn;

        if (this->writeTxn)
            txn = this->writeTxn->txn;
        else if (this->writeWorker) {
            // try to acquire the txn from the current batch
            txn = this->writeWorker->AcquireTxn(&flags);
            //fprintf(stderr, "acquired %p %p %p\n", this->writeWorker, txn, flags);
        } else {
            pthread_mutex_lock(this->writingLock);
            txn = nullptr;
        }

        if (txn) {
            if (flags & TXN_ABORTABLE) {
                if (envFlags & MDB_WRITEMAP)
                    flags &= ~TXN_ABORTABLE;
                else {
                    // child txn
                    mdb_txn_begin(env, txn, flags & 0xf0000, &txn);
                    TxnTracked* childTxn = new TxnTracked(txn, flags);
                    childTxn->parent = this->writeTxn;
                    this->writeTxn = childTxn;
                    return;
                }
            }
        } else {
            mdb_txn_begin(env, nullptr, flags & 0xf0000, &txn);
            flags |= TXN_ABORTABLE;
        }
        this->writeTxn = new TxnTracked(txn, flags);
        return;
    }

    if (info.Length() > 1) {
        const int argc = 3;

        Local<Value> argv[argc] = { info.This(), info[0], info[1] };
        maybeInstance = Nan::NewInstance(Nan::New(*txnCtor), argc, argv);

    } else {
        const int argc = 2;

        Local<Value> argv[argc] = { info.This(), info[0] };
        //fprintf(stdout, "beginTxn %u", info[0]->IsTrue());
        maybeInstance = Nan::NewInstance(Nan::New(*txnCtor), argc, argv);
    }

    // Check if txn could be created
    if ((maybeInstance.IsEmpty())) {
        // The maybeInstance is empty because the txnCtor called throwError.
        // No need to call that here again, the user will get the error thrown there.
        return;
    }

    Local<Object> instance = maybeInstance.ToLocalChecked();
    info.GetReturnValue().Set(instance);
}
napi_value EnvWrap::commitTxn(const CallbackInfo& info) {
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    TxnTracked *currentTxn = this->writeTxn;
    //fprintf(stderr, "commitTxn %p\n", currentTxn);
    int rc = 0;
    if (currentTxn->flags & TXN_ABORTABLE) {
        //fprintf(stderr, "txn_commit\n");
        rc = mdb_txn_commit(currentTxn->txn);
    }
    this->writeTxn = currentTxn->parent;
    if (!this->writeTxn) {
        //fprintf(stderr, "unlock txn\n");
        if (this->writeWorker)
            this->writeWorker->UnlockTxn();
        else
            pthread_mutex_unlock(this->writingLock);
    }
    delete currentTxn;
    if (rc)
        throwLmdbError(info.Env(), rc);
}
napi_value EnvWrap::abortTxn(const CallbackInfo& info) {
    EnvWrap *ew = Nan::ObjectWrap::Unwrap<EnvWrap>(info.This());
    TxnTracked *currentTxn = this->writeTxn;
    if (currentTxn->flags & TXN_ABORTABLE) {
        mdb_txn_abort(currentTxn->txn);
    } else {
        throwError("Can not abort this transaction");
    }
    this->writeTxn = currentTxn->parent;
    if (!this->writeTxn) {
        if (this->writeWorker)
            this->writeWorker->UnlockTxn();
        else
            pthread_mutex_unlock(this->writingLock);
    }
    delete currentTxn;
}
extern "C" EXTERN int commitEnvTxn(double ewPointer) {
    EnvWrap* ew = (EnvWrap*) (size_t) ewPointer;
    TxnTracked *currentTxn = ew->writeTxn;
    int rc = 0;
    if (currentTxn->flags & TXN_ABORTABLE) {
        //fprintf(stderr, "txn_commit\n");
        rc = mdb_txn_commit(currentTxn->txn);
    }
    ew->writeTxn = currentTxn->parent;
    if (!ew->writeTxn) {
        //fprintf(stderr, "unlock txn\n");
        if (ew->writeWorker)
            ew->writeWorker->UnlockTxn();
        else
            pthread_mutex_unlock(ew->writingLock);
    }
    delete currentTxn;
    return rc;
}
extern "C" EXTERN void abortEnvTxn(double ewPointer) {
    EnvWrap* ew = (EnvWrap*) (size_t) ewPointer;
    TxnTracked *currentTxn = ew->writeTxn;
    if (currentTxn->flags & TXN_ABORTABLE) {
        mdb_txn_abort(currentTxn->txn);
    } else {
        throwError("Can not abort this transaction");
    }
    ew->writeTxn = currentTxn->parent;
    if (!ew->writeTxn) {
        if (ew->writeWorker)
            ew->writeWorker->UnlockTxn();
        else
            pthread_mutex_unlock(ew->writingLock);
    }
    delete currentTxn;
}


napi_value EnvWrap::openDbi(const CallbackInfo& info) {


    const unsigned argc = 5;
    Local<Value> argv[argc] = { info.This(), info[0], info[1], info[2], info[3] };
    Nan::MaybeLocal<Object> maybeInstance = Nan::NewInstance(Nan::New(*dbiCtor), argc, argv);

    // Check if database could be opened
    if ((maybeInstance.IsEmpty())) {
        // The maybeInstance is empty because the dbiCtor called throwError.
        // No need to call that here again, the user will get the error thrown there.
        return;
    }

    Local<Object> instance = maybeInstance.ToLocalChecked();
    DbiWrap *dw = Nan::ObjectWrap::Unwrap<DbiWrap>(instance);
    if (dw->dbi == (MDB_dbi) 0xffffffff)
        info.GetReturnValue().Set(Nan::Undefined());
    else
        info.GetReturnValue().Set(instance);
}

napi_value EnvWrap::sync(const CallbackInfo& info) {

    if (!ew->env) {
        return throwError("The environment is already closed.");
    }
    if (info.Length() > 0) {
        Function* callback = new Function(
        Local<v8::Function>::Cast(info[0])
        );

        SyncWorker* worker = new SyncWorker(
        ew->env, callback
        );

        AsyncQueueWorker(worker);
    } else {
        int rc = mdb_env_sync(ew->env, 1);
        if (rc != 0) {
            throwLmdbError(info.Env(), rc);
        }
    }
    return;
}

napi_value EnvWrap::resetCurrentReadTxn(const CallbackInfo& info) {
    mdb_txn_reset(ew->currentReadTxn);
    ew->readTxnRenewed = false;
}

void EnvWrap::setupExports(Env env, Object exports) {
    // EnvWrap: Prepare constructor template
    Function EnvClass = DefineClass(env, "Env", {
        EnvWrap::InstanceMethod("open", &EnvWrap::open),
        EnvWrap::InstanceMethod("getMaxKeySize", &EnvWrap::getMaxKeySize),
        EnvWrap::InstanceMethod("close", &EnvWrap::close),
        EnvWrap::InstanceMethod("beginTxn", &EnvWrap::beginTxn),
        EnvWrap::InstanceMethod("commitTxn", &EnvWrap::commitTxn),
        EnvWrap::InstanceMethod("abortTxn", &EnvWrap::abortTxn),
        EnvWrap::InstanceMethod("openDbi", &EnvWrap::openDbi),
        EnvWrap::InstanceMethod("sync", &EnvWrap::sync),
        EnvWrap::InstanceMethod("startWriting", &EnvWrap::startWriting),
        EnvWrap::InstanceMethod("compress", &EnvWrap::compress),
        EnvWrap::InstanceMethod("stat", &EnvWrap::stat),
        EnvWrap::InstanceMethod("freeStat", &EnvWrap::freeStat),
        EnvWrap::InstanceMethod("info", &EnvWrap::info),
        EnvWrap::InstanceMethod("readerCheck", &EnvWrap::readerCheck),
        EnvWrap::InstanceMethod("readerList", &EnvWrap::readerList),
        EnvWrap::InstanceMethod("resize", &EnvWrap::resize),
        EnvWrap::InstanceMethod("copy", &EnvWrap::copy),
        EnvWrap::InstanceMethod("detachBuffer", &EnvWrap::detachBuffer),
        EnvWrap::InstanceMethod("resetCurrentReadTxn", &EnvWrap::resetCurrentReadTxn),
    });
    //envTpl->InstanceTemplate()->SetInternalFieldCount(1);
    exports.Set("Env", EnvClass);

    // TxnWrap: Prepare constructor template
    Function TxnClass = DefineClass(env, "Txn", {
        // TxnWrap: Add functions to the prototype
        TxnWrap::InstanceMethod("commit", &TxnWrap::commit),
        TxnWrap::InstanceMethod("abort", &TxnWrap::abort),
        TxnWrap::InstanceMethod("reset", &TxnWrap::reset),
        TxnWrap::InstanceMethod("renew", &TxnWrap::renew),
    });
    exports.Set("Txn", TxnClass);
    //txnTpl->InstanceTemplate()->SetInternalFieldCount(1);

    // TODO: wrap mdb_cmp too
    // TODO: wrap mdb_dcmp too
    // TxnWrap: Get constructor
    /*EnvWrap::txnCtor = new Nan::Persistent<Function>();
    EnvWrap::txnCtor->Reset( txnTpl->GetFunction(Nan::GetCurrentContext()).ToLocalChecked());*/
    Function DbiClass = DefineClass(env, "Dbi", {

        // DbiWrap: Prepare constructor template
        // DbiWrap: Add functions to the prototype
        TxnWrap::InstanceMethod("close", &DbiWrap::close),
        TxnWrap::InstanceMethod("drop", &DbiWrap::drop),
        TxnWrap::InstanceMethod("stat", &DbiWrap::stat),
        TxnWrap::InstanceMethod("getStringByBinary", &DbiWrap::getStringByBinary),
        TxnWrap::InstanceMethod("getSharedByBinary", &DbiWrap::getSharedByBinary),
        TxnWrap::InstanceMethod("prefetch", &DbiWrap::prefetch),

    });
    #if ENABLE_FAST_API && NODE_VERSION_AT_LEAST(16,6,0)
    auto getFast = CFunction::Make(DbiWrap::getByBinaryFast);
    TxnWrap::InstanceMethod("getByBinary", v8::FunctionTemplate::New(
          isolate, DbiWrap::getByBinary, v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasNoSideEffect, &getFast));
    auto writeFast = CFunction::Make(EnvWrap::writeFast);
    EnvWrap::InstanceMethod("write", v8::FunctionTemplate::New(
        isolate, EnvWrap::write, v8::Local<v8::Value>(),
        v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
        v8::SideEffectType::kHasNoSideEffect, &writeFast));

    #else
    TxnWrap::InstanceMethod("getByBinary", v8::FunctionTemplate::New(
          isolate, DbiWrap::getByBinary, v8::Local<v8::Value>(),
          v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
          v8::SideEffectType::kHasNoSideEffect));
    EnvWrap::InstanceMethod("write", v8::FunctionTemplate::New(
        isolate, EnvWrap::write, v8::Local<v8::Value>(),
        v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow,
        v8::SideEffectType::kHasNoSideEffect));
    #endif
//    dbiTpl->InstanceTemplate()->SetInternalFieldCount(1);


    // TODO: wrap mdb_stat too
    // DbiWrap: Get constructor
    /*EnvWrap::dbiCtor = napi_create_reference(env, new Nan::Persistent<Function>());
    EnvWrap::dbiCtor->Reset( dbiTpl->GetFunction(Nan::GetCurrentContext()).ToLocalChecked());*/
    Function CompressionClass = DefineClass(env, "Compression", {
        CompressionWrap::InstanceMethod("setBuffer", &Compression::setBuffer),
    });
    exports.Set("Compression", CompressionClass);
//    compressionTpl->InstanceTemplate()->SetInternalFieldCount(1);
}

extern "C" EXTERN int64_t envOpen(int flags, int jsFlags, char* path, char* keyBuffer, double compression, int maxDbs,
        int maxReaders, double mapSize, int pageSize, char* encryptionKey) {
    EnvWrap* ew = new EnvWrap();
    int rc = mdb_env_create(&(ew->env));
    if (rc)
        return rc;
    rc = ew->openEnv(flags, jsFlags, path, keyBuffer, (Compression*) (size_t) compression,
        maxDbs, maxReaders, (mdb_size_t) mapSize, pageSize, encryptionKey);
    if (rc)
        return rc;
    return (ssize_t) ew;
}

extern "C" EXTERN uint32_t getMaxKeySize(double ew) {
    return mdb_env_get_maxkeysize(((EnvWrap*) (size_t) ew)->env);
}
extern "C" EXTERN int32_t readerCheck(double ew) {
    int rc, dead;
    rc = mdb_reader_check(((EnvWrap*) (size_t) ew)->env, &dead);
    return rc || dead;
}
extern "C" EXTERN int64_t openDbi(double ewPointer, int flags, char* name, int keyType, double compression) {
    EnvWrap* ew = (EnvWrap*) (size_t) ewPointer;
    DbiWrap* dw = new DbiWrap(ew->env, 0);
    dw->ew = ew;
    if (((size_t) name) < 100) // 1 means nullptr?
        name = nullptr;
    int rc = dw->open(flags & ~HAS_VERSIONS, name, flags & HAS_VERSIONS,
        (LmdbKeyType) keyType, (Compression*) (size_t) compression);
    if (rc) {
        delete dw;
        return rc;
    }
    return (int64_t) dw;
}

extern "C" EXTERN int64_t beginTxn(double ewPointer, int flags) {
    EnvWrap* ew = (EnvWrap*) (size_t) ewPointer;
    TxnWrap* tw = new TxnWrap(ew->env, nullptr);
    int rc = tw->begin(ew, flags);
    if (rc) {
        delete tw;
        return rc;
    }
    return (int64_t) tw;
}
extern "C" EXTERN int32_t envSync(double ew) {
    return mdb_env_sync(((EnvWrap*) (size_t) ew)->env, 1);
}


// This file contains code from the node-lmdb project
// Copyright (c) 2013-2017 Timur Kristóf
// Copyright (c) 2021 Kristopher Tate
// Licensed to you under the terms of the MIT license
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

