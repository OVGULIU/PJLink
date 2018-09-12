// This is a direct python port of JLink. We'll see how it goes.

/********************************************************************

   JLinkNativeLibrary.c - source file for the J/Link native library

   J/Link source code (c) 1999-2004, Wolfram Research, Inc. All rights reserved.

   Use is governed by the terms of the J/Link license agreement, which can be
   found at www.wolfram.com/solutions/mathlink/jlink.

   Author: Todd Gayley

*********************************************************************/

/* TODO

    Not yet Unicode savvy for MLCheckFunction, MLCheckFunctionWithArgCount,
    MLGetArray, MLPutArray.

    Have Java-side yield func passed in as argument. This will remove
    all vestiges of a dependence on the properties of the class that
    hosts the native methods.
*/


//#define JDEBUGLEVEL 0

#include <cstdint>
#include <cerrno>
#include <csignal>
#include <cstdlib>

#if defined(DARWIN_MATHLINK) || defined(X86_DARWIN_MATHLINK) || defined(X86_64_DARWIN_MATHLINK)
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "mathlink.h"
#include "Python.h"
#include "PJLink_NativeLink.h"

//#ifdef WINDOWS_MATHLINK
///* On Windows, need to load jawt.dll to get HWNDs from a Java window.
//   Load the DLL dynamically to avoid fatal problems that have occurred in
//   non-standard runtime environments where jawt.dll cannot be found.
//*/
//#include <jawt.h>
//#include <jawt_md.h>
//HINSTANCE hJawtLib = NULL;
//typedef jboolean (JNICALL *GET_AWT_PROC) (JNIEnv* env, JAWT* awt);
//GET_AWT_PROC getAWTproc = NULL;
//#endif

/********************************** PYTHONLEVEL MACROS *****************************************************/
// // [TODO] Need to come back to this after I understand the python API better
// // It looks like the idea is just to insert the boilerplate for a NativeLink call in there
//#define MLFUNC(meth)        JNICALL Java_com_wolfram_jlink_NativeLink_##meth(JNIEnv *env, jobject ml
//#define MLFUNC(meth)  JNICALL Java_com_wolfram_jlink_NativeLink_##meth(JNIEnv *env, jclass clz
// // This is my guess for how this should work:

#define __glue(meth, ...) PyObject * PJLink_##meth(PyObject* self, ##__VA_ARGS__)
#define __glueargs(meth, ...) __glue(meth, ##__VA_ARGS__, PyObject* args)
#define __gluekwargs(meth, ...) __glue(meth, ##__VA_ARGS__, PyObject* args, PyObject* kwargs)

#define MLFUNCNOARGS(meth) __glue(meth)
#define MLFUNCWITHARGS(meth) __glueargs(meth)
#define MLFUNCWITHKWARGS(meth) __gluekwargs(meth)

#define MLPARSEARGS(...) if ( !PyArg_ParseTuple(args, __VA_ARGS__) ) return NULL;
#define MLCHECKERROR() if (PyErr_Occurred()) return NULL;
#define MLSETERROR(err, errMsgOut) if ( _MLSetError(err, errMsgOut) == -1 ) return NULL;
#define MLRETURNINT(pkt) return Py_BuildValue("i", pkt);
#define MLRETURNFLOAT(pkt) return Py_BuildValue("f", pkt);
#define MLRETURNSTR(car) return Py_BuildValue("s", car);
#define MLRETURNPTR(ptr) return PyLong_FromVoidPtr(ptr);
#define MLRETURNBOOL(boo) if (boo) { Py_RETURN_TRUE; } else { Py_RETURN_FALSE; };

/***********************************************************************************************************/

/* // JAVA DEBUGGING

#if JDEBUGLEVEL > 0
#  define DEBUGSTR1(x) DEBUGSTR(x)
#else
#  define DEBUGSTR1(x)
#endif

#if JDEBUGLEVEL > 1
#  define DEBUGSTR2(x) DEBUGSTR(x)
#else
#  define DEBUGSTR2(x)
#endif

// // DEBUGSTR should never appear in the code, only DEBUGSTR1 or DEBUGSTR2
//#ifdef WINDOWS_MATHLINK
//#  include <windows.h>
//#  define DEBUGSTR(x) MessageBox(NULL, x, "MathLinkJavaLibrary Debug", MB_OK);
//#else
//#  define DEBUGSTR(x)
//#endif

*/

/* Must be in sync with Python code. */
#define TYPE_BOOLEAN    -1
#define TYPE_BYTE		-2
#define TYPE_CHAR		-3
#define TYPE_SHORT		-4
#define TYPE_INT		-5
#define TYPE_LONG		-6
#define TYPE_FLOAT		-7
#define TYPE_DOUBLE		-8
#define TYPE_STRING		-9

#define MLE_LINK_IS_NULL        MLEUSER
#define MLE_MEMORY              MLEUSER + 1
#define MLE_ARRAY_TOO_SHALLOW   MLEUSER + 2   /* Requested array is deeper than actual */

/* Python pointer cast */

/*#ifdef _64BIT
#  define JLONG_FROM_PTR(x) ((jlong) (x))
#  define PTR_FROM_JLONG(x) ((void*) (x))
#else
#  define JLONG_FROM_PTR(x) ((jlong) (int) (x))
#  define PTR_FROM_JLONG(x) ((void*) (int) (x))
#endif*/

#ifdef _64BIT
#  define LONG_FROM_PTR(x) ((long) (x))
#  define PTR_FROM_LONG(x) ((void*) (x))
#else
#  define LONG_FROM_PTR(x) ((long) (int) (x))
#  define PTR_FROM_LONG(x) ((void*) (int) (x))  // should I really be doing this?
#endif

#define PYINT_FROM_PTR(x) PyLong_FromVoidPtr(x)
#define PTR_FROM_PYINT(x) PyLong_AsVoidPtr(x)

/* Pre declare some stuff here for use later */

static PyObject* MakeArrayN(int type, int depth, int* dims, int curDepth, int lenInFinalDim, const void* startAddr);
static PyObject* MakeArray1(int type, int len, const void* startAddr);

/*struct cookie {
    MLYieldFunctionObject yielder;
    MLMessageHandlerObject msgHandler;
    JavaVM* jvm;
    jobject ml;
    jmethodID yieldFunction;
    jmethodID messageHandler;
    int useJavaYielder;
    int useJavaMsgHandler;
};*/

struct cookie {
    MLYieldFunctionObject yielder;
    MLMessageHandlerObject msgHandler;
    PyObject *ml;
    PyObject *yieldFunction;
    PyObject *messageHandler;
    int useNumPy;
    int usePythonYielder;
    int usePythonMsgHandler;
};

enum ctype {
    kMsg,
    kYield
};
typedef enum ctype CallbackType;

/* Other things I don't yet know what I do with */
static int initEnvironment(void);
static void destroyEnvironment(void);
static void setupUserData(MLINK link, MLEnvironment env, PyObject* ml);
MLMDECL(void, msg_handler, (MLINK, int, int));
MLYDECL(devyield_result, yield_func, (MLINK, MLYieldParameters));
void setupCallback(PyObject *ml, CallbackType type, int revoke);

/* gWasTerminated is like the MLDone variable in mprep-generated installable C programs. It communicates from the
   messagehandler to the yielder that a terminate request has arrived from Mathematica.
*/
int gWasTerminated = 0;

/* All links share the same env. */
MLEnvironment gMLEnv = (MLEnvironment) 0;
/* Ref count for gMLEnv variable. */
int gEnvUseCount = 0;

/********************************  Library Management  ************************************/

/* These functions only called by 1.2 and later VMs. */

/*JAVA ONLY JNI_OnLoad
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {

    return JNI_VERSION_1_2;
}*/

/*JAVA ONLY JNI_OnUnload
JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {

//       Last chance to call MLEnd() and free MathLink resources (e.g., bug 59200).
//       In normal use, the ref-counting mechanism for gMLEnv will have already
//       called MLEnd() after the last link was closed. But we do it here in case
//       there are circumstances where this unload func is called but some links
//       still exist that haven't been closed. Just about the only way I can
//       imagine that is if a programmer didn't manually a link and its finalizer
//       wasn't called, but this unload func _was_ called. Don't know if that's
//       possible, though.
    if (gMLEnv != (MLEnvironment) 0) {
        MLEnd(gMLEnv);
        gMLEnv = (MLEnvironment) 0;
    }
}*/

/*************************  Python API Convenience Functions  **************************/

// Every one of these should start with _ML

int ML_DEBUG_LEVEL = 0;
int ML_MESSAGE_DEBUG_LEVEL = 2;
int _MLDebugPrint(int level, const char *fmt, ...) {
    if (level <= ML_DEBUG_LEVEL) {
        va_list args;
        va_start(args, fmt);
        int res = vprintf(fmt, args);
        printf("\n");
        return res;
    } else {
        return 0;
    }
}
int _MLDebugMessage(int level, const char *msg) {
//    // this is code duplication but for some reason _MLDebugMessage(lvl, msg) = _MLDebugPrint(lvl, "%s", msg) wasn't working...
//    if (level <= ML_DEBUG_LEVEL) {
//        int res = printf("%s\n", msg);
//        return res;
//    } else {
//        return 0;
//    };
    return _MLDebugPrint(level, "%s", msg);
}
int _MLDebugMessage(const char *msg) {
    return _MLDebugMessage(ML_MESSAGE_DEBUG_LEVEL, msg);
}
int _MLDebugPrintNullLink(int level) {
    return _MLDebugMessage(level, "Link is Null");
}
int _MLDebugPrintNullLink() {
    return _MLDebugMessage("Link is Null");
}

int _MLSetError ( int err, PyObject *errMsgOut) {
    const char *errMsg;
    errMsg=MLErrorString(gMLEnv, err);
    PyObject *errString;
    errString = Py_BuildValue("s", errMsg);
    int res;
    if (PyObject_Size(errMsgOut) > 0) {
        res = PyList_SetItem(errMsgOut, 0, errString);
    } else {
        res = PyList_Append(errMsgOut, errString);
        Py_XDECREF(errString);
    };

    return res;
}

const char *_MLGetString( PyObject* s, const char *enc, const char *err) {
    PyObject *pyStr;
    pyStr = PyUnicode_AsEncodedString(s, enc, err);
    if (pyStr == NULL) return NULL;
    const char *strExcType =  PyBytes_AsString(pyStr);
    Py_XDECREF(pyStr);
    return strExcType;
}
const char *_MLGetString( PyObject* s) {
    return _MLGetString( s, "utf-8", "Error~");
}
const unsigned short *_MLGetUCS2String( PyObject* s, Py_ssize_t *len) {
//    PyObject *ucs2 = PyUnicode_AsEncodedString(s, "utf16", "Error~");
//    if ( ucs2 == NULL) return NULL;
//    const char *str =  PyBytes_AsString(ucs2);
//    Py_ssize_t plen = PyObject_Size(ucs2);
//    *len = plen;
//    const unsigned short *buff = (unsigned short *) str;
    if ( ML_DEBUG_LEVEL > 4 ) {
        const char *str = _MLGetString(s);
        _MLDebugPrint(5, "Getting UCS2 object from string %s", str);
    }

//    const unsigned short *buff = (unsigned short *) PyUnicode_AsWideCharString(s, len);
    const unsigned short *buff = (unsigned short *) PyUnicode_2BYTE_DATA(s);
    MLCHECKERROR();
    _MLDebugPrint(5, "Got UCS2 string %ls", buff);
//    const unsigned short *buff = (unsigned short *) _MLGetString(ucs2, "utf16", "Error~");
    *len = PyObject_Size(s);
//    Py_DECREF(ucs2);
    return buff;
}
/*Useless _MLGetPythonUTF16String
PyObject *_MLGetPythonUTF16String(const unsigned short *s, Py_ssize_t len) {
    unsigned short tmp_s[len+1];
//    printf("len::%d\n", len);
    for (int i=0; i < len; i++){
        tmp_s[i] = s[i];
    }
    tmp_s[len] = 0;
    _MLDebugPrint(4, "Getting UTF16 object from string %hs of length %d", tmp_s, len);
    char32_t* us = (char32_t*) malloc(2*len*sizeof(char32_t));
    for (int i = 0; i < 2*len; i++) {
        us[i++] = (char32_t) 0;
        us[i] = (char32_t) s[i]; // have to upcast UCS2 since python expects UCS4
    }
//        for (int i = 0; i < len; i++) {
//            printf("%#x", us[i]);
//        }
    PyObject *str = Py_BuildValue("u", us);
    free(us);
    return str;
}*/
const char *_MLGetUTF8String( PyObject* s, Py_ssize_t *len) {
    PyObject *pyStr;
    pyStr = PyUnicode_AsEncodedString(s, "utf8", "Error~");
    if (pyStr == NULL) return NULL;
    *len = PyObject_Size(pyStr);
    const char *chars =  PyBytes_AsString(pyStr);
    Py_XDECREF(pyStr);
    return chars;
}

PyObject *_MLDecodePythonUTF16(const unsigned short *s, Py_ssize_t len, Py_ssize_t charn) {
//    const char *errors;
//    int *bom;
//    return PyUnicode_DecodeUTF16((const char *) s, len, errors, bom);
//    return PyUnicode_FromWideChar((const wchar_t*) &s[1], charn); // s[1] is pointer to next element in the array?
    _MLDebugPrint(5, "Getting UTF16 object from string of length %d", len);
    int bo = 1; // MathLink is little endian?

    const char *obuff = (const char *) s;
//    char buff[2*len];// = (char *) malloc(2*len*sizeof(char));
//    char tmp;
//    int true_len = 0;
//    for ( int i = 0; i < 2*len-1; i++){
//        tmp = obuff[i];
//        if ( tmp > 0 ) {
//            true_len ++;
//            buff[i] = tmp;
//        }
//    }
//    buff[true_len+1] = obuff[2*len];
//    free(obuff);
//    if ( len > 2 ){
//        char tmp = obuff[2];
//        char tmp2 = obuff[3];
//        if ( tmp == 0 && tmp2 == 0 ) {
//            len = 2*len; //handling some weird bug where Mathematica inserts a bunch of null chars
//        }
//    }
    PyObject *str = PyUnicode_DecodeUTF16(obuff, 2*len, "~err~", NULL);// &bo);
//    free(&buff);
    return str;
}

int _MLGetStringLength( PyObject* s, const char *enc, const char *err) {
    PyObject *pyStr;
    pyStr = PyUnicode_AsEncodedString(s, enc, err);
    int len = PyObject_Size(pyStr);
    Py_XDECREF(pyStr);
    return len;
}
int _MLGetStringLength( PyObject* s) {
    int len = PyObject_Size(s);
    return len;
}

PyObject *_MLAttachLink( PyObject *ml, MLINK mlink ) {

    // Create a capsule containing the x pointer
    if (mlink == NULL) {
        PyErr_SetString(PyExc_ValueError, "MathLink object is null");
        return NULL;
    }
    PyObject *link_cap = PyCapsule_New((void *)mlink, "_MLINK", NULL);
    _MLDebugPrint(4, "Attaching link MathLink(%p) to MathLink capsule", mlink);
    // and add it to the MathLink
    if (link_cap == NULL) {
        PyErr_SetString(PyExc_TypeError, "Couldn't create new MathLink capsule object");
        return NULL;
    } else if (!PyCapsule_IsValid(link_cap, "_MLINK")) {
        PyErr_SetString(PyExc_ValueError, "Couldn't add MathLink pointer to invalid capsule object");
        Py_XDECREF(link_cap);
        return NULL;
    }
    if (PyObject_SetAttrString(ml, "_MLINK", link_cap) == -1){
        Py_DECREF(link_cap);
        PyErr_SetString(PyExc_AttributeError, "Couldn't set '_MLINK' attribute to MathLink pointer");
        return NULL;
    } else{
        // return the pointer?
        return link_cap;
    }

}

MLINK _MLGetMLINK( PyObject *ml ) {
    PyObject *link_cap;

    link_cap = PyObject_GetAttrString(ml, "_MLINK");
    if ( link_cap == NULL ) {
        _MLDebugMessage(4, "Link capsule is NULL");
        return 0;
    } else {
        MLINK mlink = (MLINK) PyCapsule_GetPointer(link_cap, "_MLINK");
        _MLDebugPrint(4, "Extracted link MathLink(%p) from MathLink capsule", mlink);
        // Py_DECREF(link_cap);
        return mlink;
    }
}

PyObject *_MLMakeNewArray ( int type, int len ) {
    // Initialize an empty array.array type object of proper type
    static PyObject *single_array;
    if (!single_array) {
        PyObject *array_module = PyImport_ImportModule("array");
        if (!array_module)
            return NULL;
        PyObject *array_type = PyObject_GetAttrString(array_module, "array");
        Py_DECREF(array_module);
        if (!array_type)
            return NULL;
        switch (type) {
        case TYPE_CHAR:
            // array.array('c', "")
            single_array = PyObject_CallFunction(array_type, "c[c]", 'c', "");
            break;
        case TYPE_SHORT:
            // array.array('h', [0])
            single_array = PyObject_CallFunction(array_type, "c[h]", 'h', 0);
            break;
        case TYPE_INT:
            // array.array('i', [0])
            single_array = PyObject_CallFunction(array_type, "c[i]", 'i', 0);
            break;
        case TYPE_LONG:
            // array.array('l', [0])
            single_array = PyObject_CallFunction(array_type, "c[l]", 'l', 0);
            break;
        case TYPE_FLOAT:
            // array.array('f', [0.0])
            single_array = PyObject_CallFunction(array_type, "c[f]", 'f', 0.0);
            break;
        case TYPE_DOUBLE:
            // array.array('d', [0.0])
            single_array = PyObject_CallFunction(array_type, "c[d]", 'd', 0.0);
            break;
        case TYPE_BOOLEAN:

            break;
        case TYPE_BYTE:
            // array.array('h', [0])
            single_array = PyObject_CallFunction(array_type, "c[h]", 'h', 0);
            break;
        case TYPE_STRING:

            break;
        default:
            break;
        }
        Py_DECREF(array_type);
        if (!single_array)
            return NULL;
    }

    // extra-fast way to create an empty array of count elements:
    //   array = single_element_array * count
    PyObject *pysize = PyLong_FromSsize_t(len);
    if (!pysize)
        return NULL;
    PyObject *array = PyNumber_Multiply(single_array, pysize);
    Py_DECREF(single_array);
    Py_DECREF(pysize);
    if (!array)
        return NULL;

    return single_array;
}

void _MLPopulateArray( PyObject *array, const void *startAddr, int size) {

    // this is cannibalized from SO, but would be better without the memcpy
    PyObject *buffer_info = PyObject_CallMethod(array, "buffer_info", "");

    if (buffer_info) {
        PyObject *pyaddr = PyTuple_GetItem(buffer_info, 0);
        void *addr = PyLong_AsVoidPtr(pyaddr);

        // and, finally, copy the data.
        if (size) memcpy(addr, startAddr, size);
    }

}

int _MLDataSize(int type) {

    int datSize = 0;

    switch (type) {
        case TYPE_BYTE:
            datSize = sizeof(short);   /* These are the sizes of the data types in the MathLink arrays we read (so, == short for TYPE_BYTE) */
            break;
        case TYPE_CHAR:
            datSize = sizeof(int);
            break;
        case TYPE_SHORT:
            datSize = sizeof(short);
            break;
        case TYPE_INT:
            datSize = sizeof(int);
            break;
        case TYPE_FLOAT:
            datSize = sizeof(float);
            break;
        case TYPE_DOUBLE:
            datSize = sizeof(double);
            break;
        default:
            NULL;
            break;
    }

    return datSize;
}

char _MLTypeChar(int type) {

    char typeChar = 0;
    switch (type) {
        case TYPE_BYTE:
            typeChar = 'B';
            break;
        case TYPE_CHAR:
            typeChar = 'C';
            break;
        case TYPE_SHORT:
            typeChar = 'S';
            break;
        case TYPE_INT:
            typeChar = 'I';
            break;
        case TYPE_FLOAT:
            typeChar = 'F';
            break;
        case TYPE_DOUBLE:
            typeChar = 'D';
            break;
        default:
            NULL;
            break;
    }

    return typeChar;
}

Py_buffer *_MLGetDataBuffer(PyObject *data) {

    Py_buffer *view;
    if (PyObject_CheckBuffer(data)) {
        PyObject_GetBuffer(data, view, PyBUF_CONTIG_RO);
    };

    return view;

}

template<typename T>
T *_MLGetDataBufferArray(Py_buffer *view) {

    T *c_data;
    if ( view == NULL ) return NULL;
    c_data = (T *) view->buf;
    if (c_data == NULL) {
        PyBuffer_Release(view);
    }
    return c_data;

}

int _MLPutDataBuffer
    ( MLINK mlink, PyObject *data, char type, int size,
      const int *dims, const char **heads, int depth ) {

    Py_buffer *view = _MLGetDataBuffer(data);
    if (view == NULL) return 0;

    int flag = 1;
    switch (type) {
        case 'i':
            switch (size) {
                case 4:{
                    const int *c_data = _MLGetDataBufferArray<int> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutIntegerArray(mlink, c_data, (const long *)dims, heads, depth);
                    }
                    break;
                }
                case 8:{
                    const unsigned char *c_data = _MLGetDataBufferArray<unsigned char> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutInteger8Array(mlink, c_data, dims, heads, depth);
                    }
                    break;
                }
                case 16:{
                    short *c_data = _MLGetDataBufferArray<short> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutInteger16Array(mlink, c_data, dims, heads, depth);
                    }
                    break;
                }
                case 32:{
                    int *c_data = _MLGetDataBufferArray<int> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutInteger32Array(mlink, c_data, dims, heads, depth);
                    }
                    break;
                }
                case 64:{
                    long *c_data = _MLGetDataBufferArray<long> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutInteger64Array(mlink, (mlint64 *)c_data, dims, heads, depth);
                    }
                    break;
                }
                default:{
                    flag = 0;
                    break;
                }
            }
            break;
        case 'r':
            switch (size) {
                case 32:{
                    float *c_data = _MLGetDataBufferArray<float> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutReal32Array(mlink, c_data, dims, heads, depth);
                    }
                    break;
                }
                case 64:{
                    double *c_data = _MLGetDataBufferArray<double> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutReal64Array(mlink, c_data, dims, heads, depth);
                    }
                    break;
                }
                case 128: {
                    long double *c_data = _MLGetDataBufferArray<long double> (view);
                    if (c_data == NULL) {
                        flag = 0;
                    } else {
                        MLPutReal128Array(mlink, c_data, dims, heads, depth);
                    }
                    break;
                }
                default:{
                    flag = 0;
                    break;
                }
         }
        default:{
            flag = 0;
            break;
        }
    }

    PyBuffer_Release(view);
    return flag;

}

int _MLIterPut(MLINK mlink, PyObject *data, const char *head, int type){

    PyObject *iterator = PyObject_GetIter(data);
    if (iterator == NULL) {
        return 0;
    }

    int len = PySequence_Length(data);
    MLPutFunction(mlink, head, len);

    PyObject *item;
    switch (type) {
        case TYPE_BOOLEAN: {
            while ( (item = PyIter_Next(iterator)) ) {
                if (PyObject_IsTrue(item)) {
                    MLPutSymbol(mlink, "True");
                } else {
                    MLPutSymbol(mlink, "False");
                }
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_BYTE: {
            int b;
            while ( (item = PyIter_Next(iterator)) ) {
                b = (int) PyLong_AsLong(item);
                MLPutInteger(mlink, b);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_CHAR: {
            int c;
            while ( (item = PyIter_Next(iterator)) ) {
                c = (int) PyLong_AsLong(item);
                MLPutInteger(mlink, c);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_SHORT: {
            short s;
            while ( (item = PyIter_Next(iterator)) ) {
                s = (short) PyLong_AsLong(item);
                MLPutInteger16(mlink, s);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_INT: {
            int i;
            while ( (item = PyIter_Next(iterator)) ) {
                i = (int) PyLong_AsLong(item);
                MLPutInteger32(mlink, i);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_LONG: {
            long l;
            while ( (item = PyIter_Next(iterator)) ) {
                l = PyLong_AsLong(item);
                MLPutInteger64(mlink, l);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_FLOAT: {
            float f;
            while ( (item = PyIter_Next(iterator)) ) {
                f = (float) PyFloat_AsDouble(item);
                MLPutReal32(mlink, f);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_DOUBLE: {
            double d;
            while ( (item = PyIter_Next(iterator)) ) {
                d = PyFloat_AsDouble(item);
                MLPutReal64(mlink, d);
                Py_DECREF(item);
            }
            break;
        }
        case TYPE_STRING: {
            const unsigned short *s;
            while ( (item = PyIter_Next(iterator)) ) {
                Py_ssize_t len;
                s = _MLGetUCS2String(item, &len);
                if (s != NULL) {
                    MLPutUCS2String(mlink, s, len);
                } else {
                    MLPutSymbol(mlink, "Null");
                }
                Py_DECREF(item);
                // PyMem_Free(s); // whenever _MLGetUCS2String is called we need this
            }
            break;
        }
        default:
            break;
    }

    if (PyErr_Occurred()) {
        return 0;
    }

    Py_DECREF(iterator);

    return 1;
}
/******************************  MathLink Functions  ********************************/

/*MLInitialize*/
MLFUNCWITHARGS(MLInitialize) {

    _MLDebugMessage(2, ":Initialize:");

    initEnvironment();

    Py_RETURN_NONE;

}

/*MLOpenString

JNIEXPORT jlong MLFUNC(MLOpenString), jstring cmdLine, jobjectArray errMsgOut) {

    const char* utfString = (*env)->GetStringUTFChars(env, cmdLine, NULL);
    if (utfString == NULL) {
        return JLONG_FROM_PTR(0);
    } else {
        int err;
        MLINK link;
        DEBUGSTR1(utfString)
        if (!initEnvironment())
            return 0;
        link = MLOpenString(gMLEnv, utfString, &err);
        if (link != NULL) {
            gEnvUseCount++;
            setupUserData(link, gMLEnv, env, ml);
        } else if (err != MLEOK) {
            jstring errMsg = (*env)->NewStringUTF(env, MLErrorString(gMLEnv, err));
            (*env)->SetObjectArrayElement(env, errMsgOut, 0, errMsg);
        }
        (*env)->ReleaseStringUTFChars(env, cmdLine, utfString);
        return JLONG_FROM_PTR(link);
    }
}

*/

MLFUNCWITHARGS(MLOpenString) {

    _MLDebugMessage(2, ":OpenString:");

    const char *cmdLine;
    PyObject *ml, *errMsgOut;
    MLPARSEARGS("OsO", &ml, &cmdLine, &errMsgOut)

    int err;
    MLINK link;
    // DEBUGSTR1(utfString)
    if (!initEnvironment())
        return 0;

    link = MLOpenString(gMLEnv, cmdLine, &err);
    if (link != NULL) {
        gEnvUseCount++;
        setupUserData(link, gMLEnv, ml);
        // Py_DECREF(ml);
        // Py_DECREF(errMsgOut);
    } else if (err != MLEOK) {
        MLSETERROR(err, errMsgOut)
    }

    _MLDebugPrint(3, "Opened link MathLink(%p)", link);

    PyObject *cap = _MLAttachLink(ml, link);
    if (cap == NULL) return NULL;

    PyObject *link_ptr = PYINT_FROM_PTR(link);
    PyObject *tup = PyTuple_Pack(2, cap, link_ptr);
    Py_DECREF(cap);
    Py_DECREF(link_ptr);

    return tup;

}

/*MLOpen
JNIEXPORT jlong MLFUNC(MLOpen), jint argc, jobjectArray argv, jobjectArray errMsgOut) {

    int err;
    int i;
    const char *c_argv[32];  // More than enough for any argv.
    MLINK link;

    int len = (*env)->GetArrayLength(env, argv);
    for (i = 0; i < len && i < argc && i < 32; i++) {
        jobject obj = (*env)->GetObjectArrayElement(env, argv, i);
        if (obj == NULL) {
                        return PYINT_FROM_PTR(0);
        }
        c_argv[i] = (*env)->GetStringUTFChars(env, (jstring) obj, NULL);
        if (c_argv[i] == NULL) {
            // Yes, we may fail to call RELEASESTRINGUTFCHARS here...
                        return PYINT_FROM_PTR(0);
        }
    }
    if (!initEnvironment())
        return 0;
    link = MLOpenInEnv(gMLEnv, i, (char **) c_argv, &err);
    if (link != NULL) {
        gEnvUseCount++;
        setupUserData(link, gMLEnv, env, ml);
    } else if (err != MLEOK) {
        jstring errMsg = (*env)->NewStringUTF(env, MLErrorString(gMLEnv, err));
        (*env)->SetObjectArrayElement(env, errMsgOut, 0, errMsg);
    }
    while (--i >= 0) {
        (*env)->ReleaseStringUTFChars(env, (jstring) (*env)->GetObjectArrayElement(env, argv, i), c_argv[i]);
    }
        return PYINT_FROM_PTR(link);
}
*/

MLFUNCWITHARGS(MLOpen) {

    _MLDebugMessage(2, ":Open:");

    int argc;
    PyObject *ml, *argv, *errMsgOut;

    MLPARSEARGS("OiOO", &ml, &argc, &argv, &errMsgOut)

    int err;
    int i;
    char *c_argv[32];  /* More than enough for any argv. */
    MLINK link;

    // More idiomatic for Python

    PyObject *iterator = PyObject_GetIter(argv);
    if (iterator == NULL) return NULL;

    PyObject *item;

    i=0;
    const char *argstr;
    while ( (item = PyIter_Next(iterator)) ) {

        if (item == NULL) {
            // Presumably this should never happen, but just in case...
            return PYINT_FROM_PTR(0);
        }

        // Add char* to argv
        argstr=_MLGetString(item);
        if ( argstr == NULL ) return NULL;
        c_argv[i] = strdup(argstr);

        // release reference when done
        Py_DECREF(item);
        i++;
    }

    MLCHECKERROR()

    Py_DECREF(iterator);

    if (!initEnvironment())
        return Py_BuildValue("i", 0);

    link = MLOpenInEnv(gMLEnv, i, c_argv, &err);

    for (int j = 0; j < i; j++){
        free(c_argv[j]);
    }

    if (link != NULL) {
        gEnvUseCount++;
        setupUserData(link, gMLEnv, ml);
        Py_DECREF(ml);
        Py_DECREF(errMsgOut);
    } else if (err != MLEOK) {
        MLSETERROR(err, errMsgOut)
    }

    _MLDebugPrint(3, "Opened link MathLink(%p)", link);

    PyObject *cap = _MLAttachLink(ml, link);
    if (cap == NULL) return NULL;

    PyObject *link_ptr = PYINT_FROM_PTR(link);
    PyObject *tup = PyTuple_Pack(2, cap, link_ptr);
    Py_DECREF(cap);
    Py_DECREF(link_ptr);

    return tup;
}

/*MLLoopbackOpen
JNIEXPORT jlong MLFUNC(MLLoopbackOpen), jobjectArray errMsgOut) {

    MLINK link;
    int err;

    if (!initEnvironment())
        return 0;
    link = MLLoopbackOpen(gMLEnv, &err);
    if (link != NULL) {
        gEnvUseCount++;
        // Don't call setupUserData. No yield function for loopbacks.
        MLSetUserData(link, NULL, NULL);
    } else if (err != MLEOK) {
        jstring errMsg = (*env)->NewStringUTF(env, MLErrorString(gMLEnv, err));
        (*env)->SetObjectArrayElement(env, errMsgOut, 0, errMsg);
    }
        return PYINT_FROM_PTR(link);
}

*/

MLFUNCWITHARGS(MLLoopbackOpen) {

    _MLDebugMessage(2, ":LoopbackOpen:");

    PyObject *errMsgOut;
    PyObject *ml;
    MLPARSEARGS("OO", &ml, &errMsgOut)

    MLINK link;
    int err;

    if (!initEnvironment())
        return 0;
    link = MLLoopbackOpen(gMLEnv, &err);
    if (link != NULL) {
        gEnvUseCount++;
        // Don't call setupUserData. No yield function for loopbacks.
        MLSetUserData(link, NULL, NULL);
    } else if (err != MLEOK) {

        MLSETERROR(err, errMsgOut)

    }

    _MLDebugPrint(3, "Opened link MathLink(%p)", link);

    Py_DECREF(errMsgOut);

    PyObject *cap = _MLAttachLink(ml, link);
    if (cap == NULL) return NULL;

    PyObject *link_ptr = PYINT_FROM_PTR(link);
    PyObject *tup = PyTuple_Pack(2, cap, link_ptr);
    Py_DECREF(cap);
    Py_DECREF(link_ptr);

    return tup;

}

/*MLSetEnvIDString
JNIEXPORT void MLFUNC(MLSetEnvIDString), jstring id) {

    const char* chars = (*env)->GetStringUTFChars(env, id, NULL);
    if (chars != NULL) {
        MLSetEnvIDString(gMLEnv, chars);
        (*env)->ReleaseStringUTFChars(env, id, chars);
    }
}*/

MLFUNCWITHARGS(MLSetEnvIDString) {
    _MLDebugMessage(2, ":SetEnvIDString:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml, *strObj;
    MLPARSEARGS("O", &ml, &strObj);

    const char* id = _MLGetString(strObj);
    if (id == NULL) {
       return NULL;
    } else {
       MLSetEnvIDString(gMLEnv, id);
    }

    Py_RETURN_NONE;

}

/*MLGetLinkedEnvIDString
JNIEXPORT jstring MLFUNC(MLGetLinkedEnvIDString), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    jstring result;
    if (mlink == 0) {
        result = (*env)->NewStringUTF(env, "");
        DEBUGSTR1(" link is 0 in MLGetLinkedEnvIDString")
    } else {
        const char* s;
        MLGetLinkedEnvIDString(mlink, &s);
        result = (*env)->NewStringUTF(env, s);
        MLReleaseEnvIDString(mlink, s);
    }
    return result;
}*/

MLFUNCWITHARGS(MLGetLinkedEnvIDString) {

    PyObject *ml;
    MLPARSEARGS("O");
    MLINK mlink = _MLGetMLINK(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNSTR("");
    } else {
        const char *s;
        MLGetLinkedEnvIDString(mlink, &s);
        PyObject *result = Py_BuildValue("s", s);
        MLReleaseEnvIDString(mlink, s);
        return result;
    }

}

/*MLConnect
JNIEXPORT void MLFUNC(MLConnect), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLConnect")
    } else {
        MLConnect(mlink);
    }
}*/

MLFUNCWITHARGS(MLConnect) {

    _MLDebugMessage(2, ":Connect:");

    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    _MLDebugPrint(3, "Connecting link MathLink(%p)", mlink);

    if (mlink ==0 ) {
        _MLDebugPrintNullLink(3);
    } else {
        MLConnect(mlink);
    }

    _MLDebugPrint(3, "Connected link MathLink(%p)", mlink);

    Py_RETURN_NONE;

    }

/*MLClose
JNIEXPORT void MLFUNC(MLClose), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLClose")
    } else {
        struct cookie* c = (struct cookie*) MLUserData(mlink, NULL);
        MLSetUserData(mlink, NULL, NULL);
        if (c != NULL) {
            // A C-side yieldfunction and its accoutrements were set. Clean it up.
            MLSetYieldFunction(mlink, (MLYieldFunctionObject) NULL);
            MLSetMessageHandler(mlink, (MLMessageHandlerObject) NULL);
            MLDestroyYieldFunction(c->yielder);
            MLDestroyMessageHandler(c->msgHandler);
            if (c->ml != NULL) (*env)->DeleteGlobalRef(env, c->ml); // A Java-side handler for yield or msg was set.
            free(c);
        }
        MLClose(mlink);
        gEnvUseCount--;
        if (gEnvUseCount == 0)
            destroyEnvironment();
    }
}*/

MLFUNCWITHARGS(MLClose) {

    _MLDebugMessage(2, ":Close:");

    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink ==0 ) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Closing link MathLink(%p)", link);
        struct cookie* c = (struct cookie*) MLUserData(mlink, NULL);
        MLSetUserData(mlink, NULL, NULL);
        if (c != NULL) {
            // A C-side yieldfunction and its accoutrements were set. Clean it up.
            MLSetYieldFunction(mlink, (MLYieldFunctionObject) NULL);
            MLSetMessageHandler(mlink, (MLMessageHandlerObject) NULL);
            MLDestroyYieldFunction(c->yielder);
            MLDestroyMessageHandler(c->msgHandler);
            // if (c->ml != NULL) Py_DECREF(c->ml); // A Python-side handler for yield or msg was set.
            free(c);
        }
        MLClose(mlink);
        gEnvUseCount--;
        if (gEnvUseCount == 0)
            destroyEnvironment();
    }

    Py_RETURN_NONE;

}

/*MLName
JNIEXPORT jstring MLFUNC(MLName), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    const char *s;
    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLErrorMessage")
        s = "";
    } else {
        s = MLName(mlink);
    }
    return (*env)->NewStringUTF(env, s);
}*/

MLFUNCWITHARGS(MLName) {

    _MLDebugMessage(2, ":Name:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    const char *s;
    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        s = "";
    } else {
        _MLDebugPrint(3, "Getting name off link MathLink(%p)", mlink);
        s = MLName(mlink);
    }

    PyObject *name;
    name = Py_BuildValue("s", s);
    return name;

}

/*MLSetYieldFunction
JNIEXPORT void MLFUNC(MLSetYieldFunction), jlong link, jboolean revoke) {

    setupCallback(env, link, kYield, revoke);
}*/

MLFUNCWITHARGS(MLSetYieldFunction) {

    _MLDebugMessage(2, ":SetYieldFunction:");

    int revoke;
    PyObject *ml;
    MLPARSEARGS("Oi", &ml, &revoke);

    MLINK mlink = _MLGetMLINK(ml);

    if (mlink ==0 ) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Setting up yield function", mlink);
        setupCallback(ml, kYield, revoke);
    }

    Py_RETURN_NONE;

}

/*MLSetMessageHandler
JNIEXPORT void MLFUNC(MLSetMessageHandler), jlong link) {

    setupCallback(env, link, kMsg, 0);

}*/

MLFUNCWITHARGS(MLSetMessageHandler) {

    PyObject *ml;
    MLPARSEARGS("O", &ml);

    MLINK mlink = _MLGetMLINK(ml);

    if (mlink ==0 ) {
        _MLDebugPrintNullLink(3);
    } else {
        setupCallback(ml, kMsg, 0);
    }

    Py_RETURN_NONE;

}

/*MLNewPacket
JNIEXPORT void MLFUNC(MLNewPacket), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLNewPacket")
    } else {
        MLNewPacket(mlink);
    }
}*/

MLFUNCWITHARGS(MLNewPacket) {

    _MLDebugMessage(2, ":NewPacket:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink ==0 ) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Opening new packet on link MathLink(%p)", mlink);
        MLNewPacket(mlink);
    }

    Py_RETURN_NONE;

}

/*MLEndPacket
JNIEXPORT void MLFUNC(MLEndPacket), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLEndPacket")
    } else {
        MLEndPacket(mlink);
    }
}*/

MLFUNCWITHARGS(MLEndPacket) {

    _MLDebugMessage(2, ":EndPacket:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink ==0 ) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Closing packet on link MathLink(%p)", mlink);
        MLEndPacket(mlink);
    }

    Py_RETURN_NONE;

}

/*MLNextPacket
JNIEXPORT jint MLFUNC(MLNextPacket), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLNextPacket")
        return ILLEGALPKT;
    } else {
        return MLNextPacket(mlink);
    }
}*/

MLFUNCWITHARGS(MLNextPacket) {

    _MLDebugMessage(2, ":NextPacket:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNINT(ILLEGALPKT);
    } else {
        // Py_RETURN_NONE;
        _MLDebugPrint(3, "Getting next packet off link MathLink(%p)", mlink);
        int pkt = MLNextPacket(mlink);
        MLRETURNINT(pkt);
    }

}

/*MLError
JNIEXPORT jint MLFUNC(MLError), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLError")
        return MLE_LINK_IS_NULL;
    } else {
        return MLError(mlink);
    }
}*/

MLFUNCWITHARGS(MLError) {

    _MLDebugMessage(2, ":Error:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNINT(MLE_LINK_IS_NULL);
    } else {
        _MLDebugPrint(3, "Getting error on link MathLink(%p)", mlink);
        MLRETURNINT(MLError(mlink));
    }

}

/*MLErrorMessage
JNIEXPORT jstring MLFUNC(MLErrorMessage), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    const char *s;
    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLErrorMessage")
        s = "";
    } else {
        s = MLErrorMessage(mlink);
    }
    return (*env)->NewStringUTF(env, s);
}*/

MLFUNCWITHARGS(MLErrorMessage) {

    _MLDebugMessage(2, ":ErrorMessage:");

    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    const char *s;
    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        s = "";
    } else {
        _MLDebugPrint(3, "Getting error message on link MathLink(%p)", mlink);
        s = MLErrorMessage(mlink);
    }

//    Py_RETURN_NONE;
    MLRETURNSTR(s);

}

/*MLClearError
JNIEXPORT jboolean MLFUNC(MLClearError), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLClearError")
        return (jboolean) 0;
    } else {
        return (jboolean) MLClearError(mlink);
    }
}*/

MLFUNCWITHARGS(MLClearError) {

    _MLDebugMessage(2, ":ClearError:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNBOOL(false);
    } else {
        _MLDebugPrint(3, "Clearing error on link MathLink(%p)", mlink);
        MLRETURNBOOL(MLClearError(mlink));
    }

}

/*MLSetError
JNIEXPORT void MLFUNC(MLSetError), jlong link, jint err) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLSetError")
    } else {
        MLSetError(mlink, err);
    }
}*/

MLFUNCWITHARGS(MLSetError) {

    _MLDebugMessage(2, ":SetError:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int err;
    MLPARSEARGS("Oi", &ml, &err);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Seting error on MathLink(%p) to %d", mlink, err);
        MLSetError(mlink, err);
    }

    Py_RETURN_NONE;

}

/*MLReady
JNIEXPORT jboolean MLFUNC(MLReady), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLReady")
        return (jboolean) 0;
    } else {
        return (jboolean) MLReady(mlink);
    }
}*/

MLFUNCWITHARGS(MLReady) {

    _MLDebugMessage(2, ":Ready:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
         _MLDebugPrintNullLink(3);
        MLRETURNBOOL(false);
    } else {
        _MLDebugPrint(3, "Querying ready state on link MathLink(%p)", mlink);
        MLRETURNBOOL(MLReady(mlink));
    }

}

/*MLFlush
JNIEXPORT void MLFUNC(MLFlush), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLFlush")
    } else {
        MLFlush(mlink);
    }
}*/

MLFUNCWITHARGS(MLFlush) {

    _MLDebugMessage(2, ":Flush:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
         _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Flushing link MathLink(%p)", mlink);
        MLFlush(mlink);
    }

    Py_RETURN_NONE;

}

/*MLGetNext
JNIEXPORT jint MLFUNC(MLGetNext), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetNext")
        return (jint) MLTKERR;
    } else {
        return (jint) MLGetNext(mlink);
    }
}*/

MLFUNCWITHARGS(MLGetNext) {

    _MLDebugMessage(2, ":GetNext:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNINT(MLTKERR);
    } else {
        _MLDebugPrint(3, "Getting next type on link MathLink(%p)", mlink);
        MLRETURNINT(MLGetNext(mlink));
    }
}

/*MLGetType
JNIEXPORT jint MLFUNC(MLGetType), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetType")
        return (jint) MLTKERR;
    } else {
        return (jint) MLGetType(mlink);
    }
}*/

MLFUNCWITHARGS(MLGetType) {

    _MLDebugMessage(2, ":GetType:");

    // Basic structure to be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNINT(MLTKERR);
    } else {
        _MLDebugPrint(3, "Getting current type on link MathLink(%p)", mlink);
        int type = MLGetType(mlink);
        _MLDebugPrint(4, "Got type %d off link MathLink(%p)", type, mlink);
        MLRETURNINT(type);
    }

}

/*MLPutNext
JNIEXPORT void MLFUNC(MLPutNext), jlong link, jint type) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutNext")
    } else {
        MLPutNext(mlink, (int) type);
    }
}*/

MLFUNCWITHARGS(MLPutNext) {

    _MLDebugMessage(2, ":PutNext:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int type;
    MLPARSEARGS("Oi", &ml,  &type);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Putting next type (%d) on link MathLink(%p)", type, mlink);
        MLPutNext(mlink, type);
    };

    Py_RETURN_NONE;

}

/*MLGetArgCount
JNIEXPORT jint MLFUNC(MLGetArgCount), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetArgCount")
        return (jint) 0;
    } else {
        int argc;
        if (MLGetArgCount(mlink, &argc) != 0) {
            return (jint) argc;
        } else {
            return (jint) 0;
        }
    }
}*/

MLFUNCWITHARGS(MLGetArgCount) {

    _MLDebugMessage(2, ":GetArgCount:");

    // Basic structure to be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNINT(0)
    } else {
        _MLDebugPrint(3, "Getting arg count on link MathLink(%p)", mlink);
        int argc;
        if (MLGetArgCount(mlink, &argc) != 0) {
            MLRETURNINT(argc);
        } else {
            MLRETURNINT(0);
        }
    };

}

/*MLPutArgCount
JNIEXPORT void MLFUNC(MLPutArgCount), jlong link, jint cnt) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutArgCount")
    } else {
        MLPutArgCount(mlink, (int) cnt);
    }
}*/

MLFUNCWITHARGS(MLPutArgCount) {

    _MLDebugMessage(2, ":PutArgCount:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int cnt;
    MLPARSEARGS("Oi", &ml, &cnt);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Putting arg count (%d) on link MathLink(%p)", cnt, mlink);
        MLPutArgCount(mlink, cnt);
    };

    Py_RETURN_NONE;

}

/*MLGetString
JNIEXPORT jstring MLFUNC(MLGetString), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetString")
        return (*env)->NewString(env, NULL, 0);
    } else {
        const unsigned short *s;
        int len;
        jstring str;
        if (MLGetUCS2String(mlink, &s, &len) == 0) {
            DEBUGSTR1(" MLGetUnicodeString failed")
            return (*env)->NewString(env, NULL, 0);
        }
        str = (*env)->NewString(env, s, len);
        MLReleaseUCS2String(mlink, s, len);
        return str;
    }

}*/

MLFUNCWITHARGS(MLGetString) {

    _MLDebugMessage(2, ":GetString:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNSTR("");
    } else {
//        const unsigned short *s;
        const unsigned char *s;
        int len;
        int charn;
//        if (MLGetUTF16String(mlink, &s, &len, &charn) == 0) {
//        if (MLGetUCS2String(mlink, &s, &len) == 0) {
        if (MLGetUTF8String(mlink, &s, &len, &charn)  == 0) {
            _MLDebugPrint(3, "No string on link MathLink(%p)", mlink);
            MLRETURNSTR("");
        }
        _MLDebugPrint(3, "Getting string on link MathLink(%p)", mlink);
//        PyObject *str = _MLDecodePythonUTF16(s, len, charn);
        PyObject *str = Py_BuildValue("s", s);
        MLReleaseUTF8String(mlink, s, len);
//        MLReleaseUTF16String(mlink, s, len);
//        MLReleaseUCS2String(mlink, s, len);
        return str;
    }

}

/*MLPutString
JNIEXPORT void MLFUNC(MLPutString), jlong link, jstring s) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutString")
    } else if (s == NULL) {
        MLPutSymbol(mlink, "Null");
    } else {
        const jchar* chars = (*env)->GetStringChars(env, s, NULL);
        MLPutUCS2String(mlink, chars, (*env)->GetStringLength(env, s));
        (*env)->ReleaseStringChars(env, s, chars);
    }
}*/

MLFUNCWITHARGS(MLPutString) {

    _MLDebugMessage(2, ":PutString:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml, *strObj;
    MLPARSEARGS("OO", &ml, &strObj);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        if (strObj == Py_None) {
            _MLDebugPrint(3, "String is None (putting symbol Null)");
            MLPutSymbol(mlink, "Null");
        } else {
//            const unsigned short* chars;
//            Py_ssize_t len;
//            chars = _MLGetUCS2String(strObj, &len);
            Py_ssize_t len;
            const unsigned char * chars = (unsigned char *) _MLGetUTF8String(strObj, &len);
            if ( chars == NULL) {
                return NULL;
            };
            _MLDebugPrint(3, "Putting string %ls of length %d on link MathLink(%p)", chars, len, mlink);
//            MLPutUTF16String(mlink, chars, len);
//            MLPutUCS2String(mlink, chars, len);
            MLPutUTF8String(mlink, chars, len);
        }
    }

    Py_RETURN_NONE;

}

/*MLGetByteString
JNIEXPORT jbyteArray MLFUNC(MLGetByteString), jlong link, jbyte missing) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    jbyteArray res = NULL;
    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetByteString")
    } else {
        const unsigned char *s;
        int len;
        if (MLGetByteString(mlink, &s, &len, missing) == 0) {
            DEBUGSTR1(" MLGetByteString failed")
        } else {
            res = (*env)->NewByteArray(env, len);
            if (res != NULL) {
                (*env)->SetByteArrayRegion(env, res, 0, len, (jbyte*)s);
            }
            MLReleaseByteString(mlink, s, len);
        }
    }
    return res;
}*/

MLFUNCWITHARGS(MLGetByteString) {

    _MLDebugMessage(2, ":GetByteString:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    long missing;
    MLPARSEARGS("Ol", &ml, &missing);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    PyObject *bytes;
    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        bytes = Py_BuildValue("y", NULL);
    } else {
        const unsigned char *s;
        int len;
        _MLDebugPrint(3, "Getting byte string off link MathLink(%p)", mlink);
        if (MLGetByteString(mlink, &s, &len, missing) == 0) {
            _MLDebugPrint(4, "Got empty byte string off link MathLink(%p)", mlink);
            bytes = Py_BuildValue("y", NULL);
        }
        bytes = Py_BuildValue("y", s);
        MLReleaseByteString(mlink, s, len);
    }

    return bytes;

}

/*MLPutByteString
JNIEXPORT void MLFUNC(MLPutByteString), jlong link, jbyteArray data, jint len) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutByteString")
    } else if (data == NULL) {
        DEBUGSTR1(" data is null in MLPutByteString")
    } else {
        jbyte *c_data = (*env)->GetByteArrayElements(env, data, NULL);
        if (c_data == NULL) {
            DEBUGSTR1(" mem failure in MLPutByteString")
            MLSetError(mlink, MLE_MEMORY);
            return;
        }
        MLPutByteString(mlink, (unsigned char*)c_data, len);
        (*env)->ReleaseByteArrayElements(env, data, c_data, JNI_ABORT);
    }
}*/

MLFUNCWITHARGS(MLPutByteString) {

    _MLDebugMessage(2, ":PutByteString:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    Py_buffer bytes;
    // This is dangerous but potentially very useful?
    // If the bytes object may be mutated this could be highly efficient
    MLPARSEARGS("Oy*", &ml, &bytes);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        const unsigned char *c_data;
        c_data = (const unsigned char *) bytes.buf;
        Py_ssize_t len = bytes.len;
        if (c_data == NULL) {
            _MLDebugMessage(4, "Got empty byte string");
            MLSetError(mlink, MLE_MEMORY);
            return NULL;
        }
        _MLDebugPrint(3, "Putting byte string onto link MathLink(%p)", mlink);
        MLPutByteString(mlink, c_data, len);
    }

    Py_RETURN_NONE;

}

/*MLGetSymbol
JNIEXPORT jstring MLFUNC(MLGetSymbol), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetSymbol")
        return NULL;
    } else {
        const unsigned short *s;
        int len;
        jstring str;
        if (MLGetUCS2Symbol(mlink, &s, &len) == 0) {
            DEBUGSTR1(" MLGetSymbol failed")
            return NULL;
        }
        str = (*env)->NewString(env, s, len);
        MLReleaseUCS2Symbol(mlink, s, len);
        if (str == NULL) {
            DEBUGSTR1(" mem failure in MLGetSymbol")
            MLSetError(mlink, MLE_MEMORY);
        }
        return str;
    }
}*/

MLFUNCWITHARGS(MLGetSymbol) {

    _MLDebugMessage(2, ":GetSymbol:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        return NULL;
    } else {
//        const unsigned short *s;
//        int len;
//        int charn = 0;
        const unsigned char *s;
        int len;
        int charn;
        if (MLGetUTF8String(mlink, &s, &len, &charn) == 0) {
//        if (MLGetUTF16Symbol(mlink, &s, &len, &charn) == 0) {
//        if (MLGetUCS2Symbol(mlink, &s, &len) == 0) {
            _MLDebugPrint(4, "Got no symbol");
            MLRETURNSTR("");
        }
        _MLDebugPrint(3, "Getting symbol on link MathLink(%p)", mlink);
//        PyObject *str = _MLDecodePythonUTF16(s, len, charn);
        PyObject *str = Py_BuildValue("s", s);
//        MLReleaseUTF16Symbol(mlink, s, len);
        MLReleaseUTF8Symbol(mlink, s, len);
        return str;
    }

}

/*MLPutSymbol
JNIEXPORT void MLFUNC(MLPutSymbol), jlong link, jstring s) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutSymbol")
    } else if (s == NULL) {
        MLPutSymbol(mlink, "Null");
    } else {
        const jchar* chars = (*env)->GetStringChars(env, s, NULL);
        MLPutUCS2Symbol(mlink, chars, (*env)->GetStringLength(env, s));
        (*env)->ReleaseStringChars(env, s, chars);
    }
}*/

MLFUNCWITHARGS(MLPutSymbol) {

    _MLDebugMessage(":PutSymbol:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml, *strObj;
    MLPARSEARGS("OO", &ml, &strObj);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        if (strObj == Py_None) {
            _MLDebugMessage(3, "Symbol is None (putting symbol Null)");
            MLPutSymbol(mlink, "Null");
        } else {
//            const unsigned short *chars;
//            Py_ssize_t len = 0;
//            chars = _MLGetUCS2String(strObj, &len);
            Py_ssize_t len;
            const unsigned char * chars = (unsigned char *) _MLGetUTF8String(strObj, &len);
            if (chars == NULL) return NULL;
            _MLDebugPrint(3, "Putting symbol %ls of length %d on link MathLink(%p)", chars, len, mlink);
//            MLPutUTF16Symbol(mlink, chars, (int) len);
//            MLPutUCS2Symbol(mlink, chars, (int) len);
            MLPutUTF8Symbol(mlink, chars, len);
        }
    }

    Py_RETURN_NONE;

}

/*MLGetInteger
JNIEXPORT jint MLFUNC(MLGetInteger), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetInteger")
        return (jint) 0;
    } else {
        int i;
        if (MLGetInteger(mlink, &i) != 0) {
            return (jint) i;
        } else {
            return (jint) 0;
        }
    }
}*/

MLFUNCWITHARGS(MLGetInteger) {

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNINT(0);
    } else {
        int i;
        if (MLGetInteger(mlink, &i) != 0) {
            MLRETURNINT(i);
        } else {
            MLRETURNINT(0);
        }
    }

}

/*MLPutInteger
JNIEXPORT void MLFUNC(MLPutInteger), jlong link, jint i) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutInteger")
    } else {
        MLPutInteger(mlink, (int) i);
    }
}*/

MLFUNCWITHARGS(MLPutInteger) {
    _MLDebugMessage(2, ":PutInteger:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int i;
    MLPARSEARGS("Oi", &ml, &i);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Putting integer %d on link MathLink(%p)", i, mlink);
        MLPutInteger(mlink, i);
    }

    Py_RETURN_NONE;

}

/*MLGetDouble
JNIEXPORT jdouble MLFUNC(MLGetDouble), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetDouble")
        return (jdouble) 0.0;
    } else {
        double d;
        if (MLGetDouble(mlink, &d) != 0) {
            return (jdouble) d;
        } else {
            return (jdouble) 0.0;
        }
    }
}*/

MLFUNCWITHARGS(MLGetDouble) {
    _MLDebugMessage(2, ":GetDouble:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
        MLRETURNFLOAT(0.0);
    } else {
        double d;
        if (MLGetDouble(mlink, &d) != 0) {
            MLRETURNFLOAT(d);
        } else {
            MLRETURNFLOAT(0.0);
        }
    }

}

/*MLPutDouble
JNIEXPORT void MLFUNC(MLPutDouble), jlong link, jdouble d) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutDouble")
    } else {
        MLPutDouble(mlink, (double) d);
    }
}*/

MLFUNCWITHARGS(MLPutDouble) {
    _MLDebugMessage(2, ":PutDouble:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    double d;
    MLPARSEARGS("Od", &ml, &d);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink == 0) {
        _MLDebugPrintNullLink(3);
    } else {
        _MLDebugPrint(3, "Putting double %f on link MathLink(%p)", d, mlink);
        MLPutDouble(mlink, d);
    }

    Py_RETURN_NONE;

}

/*MLCheckFunction
JNIEXPORT jint MLFUNC(MLCheckFunction), jlong link, jstring s) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    // This needs to be Unicode-ified....
    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLCheckFunction")
        return (jint) 0;
    } else if (s == NULL) {
        DEBUGSTR1(" string is null in MLCheckFunction")
        return (jint) 0;
    } else {
        const char *f = (*env)->GetStringUTFChars(env, s, NULL);
        if (f == NULL) {
            MLSetError(mlink, MLE_MEMORY);
            return (jint) 0;
        } else {
            long argCount = 0;
            MLCheckFunction(mlink, f, &argCount);
            (*env)->ReleaseStringUTFChars(env, s, f);
            return (jint) argCount;
        }
    }
}*/

MLFUNCWITHARGS(MLCheckFunction) {
    _MLDebugMessage(2, ":CheckFunction:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml, *s;
    MLPARSEARGS("OO", &ml, &s);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    // This needs to be Unicode-ified....
    if (mlink == 0) {
        MLRETURNINT(0);
    } else if (s == NULL) {
        MLRETURNINT(0);
    } else {
        const char* f = _MLGetString(s);
        if (f == NULL) {
            MLSetError(mlink, MLE_MEMORY);
            MLRETURNINT(0);
        } else {
            long argCount = 0;
            MLCheckFunction(mlink, f, &argCount);
            MLRETURNINT(argCount);
        }
    };

}

/*MLCheckFunctionWithArgCount
JNIEXPORT jint MLFUNC(MLCheckFunctionWithArgCount), jlong link, jstring s, jint argc) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    // This needs to be Unicode-ified....
    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLCheckFunctionWithArgCount")
        return (jint) 0;
    } else if (s == NULL) {
        DEBUGSTR1(" string is null in MLCheckFunctionWithArgCount")
        return (jint) 0;
    } else {
        const char *f = (*env)->GetStringUTFChars(env, s, NULL);
        if (f == NULL) {
            MLSetError(mlink, MLE_MEMORY);
            return (jint) 0;
        } else {
            long argCount = (long) argc;
            MLCheckFunctionWithArgCount(mlink, f, &argCount);
            (*env)->ReleaseStringUTFChars(env, s, f);
            return (jint) argCount;
        }
    }
}*/

MLFUNCWITHARGS(MLCheckFunctionWithArgCount) {
    _MLDebugMessage(2, ":CheckFunctionWithArgCount:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml, *s;
    long argCount;
    MLPARSEARGS("O", &ml, &s, &argCount);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    // This needs to be Unicode-ified....
    if (mlink == 0) {
        MLRETURNINT(0);
    } else if (s == NULL) {
        MLRETURNINT(0);
    } else {
        const char* f = _MLGetString(s);
        if (f == NULL) {
            MLSetError(mlink, MLE_MEMORY);
            MLRETURNINT(0);
        } else {
            MLCheckFunction(mlink, f, &argCount);
            MLRETURNINT(argCount);
        }
    };

}

/*MLGetData
JNIEXPORT jbyteArray MLFUNC(MLGetData), jlong link, jint len) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    jbyteArray res = NULL;
    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetData")
    } else {
        int cnt;
        char *buf = malloc(len);
        if (buf == NULL) {
            DEBUGSTR1(" Out of memory in MLGetData")
            MLSetError(mlink, MLE_MEMORY);
        } else if (!MLGetData(mlink, buf, len, &cnt)) {
            DEBUGSTR1(" MLGetData failed")
        } else {
            res = (*env)->NewByteArray(env, cnt);
            if (res != NULL) {
                (*env)->SetByteArrayRegion(env, res, 0, cnt, (jbyte*)buf);
            }
        }
        if (buf) free(buf);
    }
    return res;
}*/

MLFUNCWITHARGS(MLGetData) {
    _MLDebugMessage(2, ":GetData:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int len;
    MLPARSEARGS("Oi", &ml, &len);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink != 0) {
        int cnt;

        char *buf = (char *) malloc(len);
        if (buf == NULL) {
            MLSetError(mlink, MLE_MEMORY);
        } else if (!MLGetData(mlink, buf, len, &cnt)) {
           NULL;
        } else {
            PyObject *res;
            res = PyByteArray_FromStringAndSize(buf, cnt);
            if (buf) free(buf);
            return res;
        }
        if (buf) free(buf);
        Py_RETURN_NONE;
    } else {
        Py_RETURN_NONE;
    }

}

/*MLPutData
JNIEXPORT void MLFUNC(MLPutData), jlong link, jbyteArray data, jint len) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutData")
    } else if (data == NULL) {
        DEBUGSTR1(" data is null in MLPutData")
    } else {
        jbyte *c_data = (*env)->GetByteArrayElements(env, data, NULL);
        if (c_data == NULL) {
            DEBUGSTR1(" mem failure in MLPutData")
            MLSetError(mlink, MLE_MEMORY);
            return;
        }
        MLPutData(mlink, (const char*)c_data, len);
        (*env)->ReleaseByteArrayElements(env, data, c_data, JNI_ABORT);
    }
}*/

MLFUNCWITHARGS(MLPutData) {
    _MLDebugMessage(2, ":PutData:");

    // Basic structure o be used whenever a link is needed

    PyObject *ml;
    Py_buffer bytes;
    // This is dangerous but potentially very useful?
    // If the bytes object may be mutated this could be highly efficient
    MLPARSEARGS("Oy*", &ml, &bytes);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink != 0) {
        const char *c_data;
        c_data = (const char *) bytes.buf;
        int64_t len = bytes.len;
        if (c_data == NULL) {
            MLSetError(mlink, MLE_MEMORY);
            return NULL;
        }
        MLPutData(mlink, c_data, len);
    }

    Py_RETURN_NONE;

}

/*MLBytesToGet
JNIEXPORT jint MLFUNC(MLBytesToGet), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLBytesToGet")
        return (jint) 0;
    } else {
        int i;
        if (MLBytesToGet(mlink, &i) != 0) {
            return (jint) i;
        } else {
            return (jint) 0;
        }
    }
}*/

MLFUNCWITHARGS(MLBytesToGet) {
    _MLDebugMessage(2, ":BytesToGet:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink == 0) {
        MLRETURNINT(0);
    } else {
        int i;
        if (MLBytesToGet(mlink, &i) != 0) {
            MLRETURNINT(i);
        } else {
            MLRETURNINT(0);
        }
    }

}

/*MLPutSize
JNIEXPORT void MLFUNC(MLPutSize), jlong link, jint len) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutSize")
    } else {
        MLPutSize(mlink, len);
    }
}*/

MLFUNCWITHARGS(MLPutSize) {
    _MLDebugMessage(2, ":PutSize:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int len;
    MLPARSEARGS("Oi", &ml, &len);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink != 0) {
        MLPutSize(mlink, len);
    }

    Py_RETURN_NONE;

}

/*MLBytesToPut
JNIEXPORT jint MLFUNC(MLBytesToPut), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLBytesToPut")
        return (jint) 0;
    } else {
        int i;
        if (MLBytesToPut(mlink, &i) != 0) {
            return (jint) i;
        } else {
            return (jint) 0;
        }
    }
}*/

MLFUNCWITHARGS(MLBytesToPut) {
    _MLDebugMessage(2, ":BytesToPut:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink == 0) {
        MLRETURNINT(0);
    } else {
        int i;
        if (MLBytesToPut(mlink, &i) != 0) {
            MLRETURNINT(i);
        } else {
            MLRETURNINT(0);
        }
    }

}

/* Maximum depth is 5 (required by C code; must be enforced by the calling Java code). */
/*MLGetArray
JNIEXPORT jobject MLFUNC(MLGetArray), jlong link, jint type, jint depth, jobjectArray headsArray) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    int i;
    int *dims;
    char **heads;
    int actualDepth, lenInLastDimension;
    jobject retval;
    void *data;
    int res;

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetArray")
        return (jobject) NULL;
    }

    switch (type) {
        case TYPE_BYTE:
        case TYPE_SHORT:
            res = MLGetInteger16Array(mlink, (short**) &data, &dims, &heads, &actualDepth);
            break;
        case TYPE_CHAR:
        case TYPE_INT:
            res = MLGetInteger32Array(mlink, (int**) &data, &dims, &heads, &actualDepth);
            break;
        case TYPE_FLOAT:
            res = MLGetReal32Array(mlink, (float**) &data, &dims, &heads, &actualDepth);
            break;
        case TYPE_DOUBLE:
            res = MLGetReal64Array(mlink, (double**) &data, &dims, &heads, &actualDepth);
            break;
        default:
            // Don't have to worry about proper cleanup here since this is only to catch bugs during development.
            DEBUGSTR1(" Bad type in MLGetArray")
            return (jobject) NULL;
    }

    if (res == 0)
        return (jobject) NULL;

    if (headsArray != NULL) {
        int headsArrayLen = (*env)->GetArrayLength(env, headsArray);
        for (i = 0; i < actualDepth && i < headsArrayLen; i++)
            (*env)->SetObjectArrayElement(env, headsArray, i, (*env)->NewStringUTF(env, heads[i]));
    }

    if (actualDepth >= depth) {
        lenInLastDimension = dims[depth-1];
        for (i = depth; i < actualDepth; i++) {
            lenInLastDimension *= dims[i];
        }
        retval = MakeArrayN(env, type, depth, dims, 0, lenInLastDimension, data);
    } else {
        // It is an error to request an array deeper than what is actually there.
        MLSetError(mlink, MLE_ARRAY_TOO_SHALLOW);
        retval = (jobject) NULL;
    }

    switch (type) {
        case TYPE_BYTE:
        case TYPE_SHORT:
            MLReleaseInteger16Array(mlink, (short*)data, dims, heads, actualDepth);
            break;
        case TYPE_CHAR:
        case TYPE_INT:
            MLReleaseInteger32Array(mlink, (int*)data, dims, heads, actualDepth);
            break;
        case TYPE_FLOAT:
            MLReleaseReal32Array(mlink, (float*)data, dims, heads, actualDepth);
            break;
        case TYPE_DOUBLE:
            MLReleaseReal64Array(mlink, (double*)data, dims, heads, actualDepth);
            break;
    }

    return retval;
}*/

MLFUNCWITHARGS(MLGetArray) {
    _MLDebugMessage(2, ":GetArray:");
    //jlong link, jint type, jint depth, jobjectArray headsArray

    PyObject *ml, *headsArray;
    int type, depth;
    MLPARSEARGS("OiiO", &ml, &type, &depth, &headsArray);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    int i;
    int *dims;
    char **heads;
    int actualDepth, lenInLastDimension;
    PyObject *retval;
    void *data;
    int res;

    if (mlink == 0) {
        Py_RETURN_NONE;
    }

    switch (type) {
        case TYPE_BYTE:
        case TYPE_SHORT:
            res = MLGetInteger16Array(mlink, (short**) &data, &dims, &heads, &actualDepth);
            break;
        case TYPE_CHAR:
        case TYPE_INT:
            res = MLGetInteger32Array(mlink, (int**) &data, &dims, &heads, &actualDepth);
            break;
        case TYPE_FLOAT:
            res = MLGetReal32Array(mlink, (float**) &data, &dims, &heads, &actualDepth);
            break;
        case TYPE_DOUBLE:
            res = MLGetReal64Array(mlink, (double**) &data, &dims, &heads, &actualDepth);
            break;
        default:
            // Don't have to worry about proper cleanup here since this is only to catch bugs during development.
            // Py_DECREF(headsArray);
            Py_RETURN_NONE;
    }

    if (res == 0){
        // Py_DECREF(headsArray);
        Py_RETURN_NONE;
    }

    if ( headsArray != Py_None ) {

        int headsArrayLen;
        headsArrayLen = PyList_Size(headsArray);
        for (i = 0; i < actualDepth && i < headsArrayLen; i++) {
            PyObject *strObj = Py_BuildValue("s", heads[i]);
            PyList_SetItem(headsArray, i, strObj);
            // Don't need decref since SetItem steals ref
        }
    }

    if (actualDepth >= depth) {
        lenInLastDimension = dims[depth-1];
        for (i = depth; i < actualDepth; i++) {
            lenInLastDimension *= dims[i];
        }
        retval = MakeArrayN(type, depth, dims, 0, lenInLastDimension, data);
    } else {
        // It is an error to request an array deeper than what is actually there.
        MLSetError(mlink, MLE_ARRAY_TOO_SHALLOW);
        // Py_DECREF(headsArray);
        Py_RETURN_NONE;
    }

    switch (type) {
        case TYPE_BYTE:
        case TYPE_SHORT:
            MLReleaseInteger16Array(mlink, (short*)data, dims, heads, actualDepth);
            break;
        case TYPE_CHAR:
        case TYPE_INT:
            MLReleaseInteger32Array(mlink, (int*)data, dims, heads, actualDepth);
            break;
        case TYPE_FLOAT:
            MLReleaseReal32Array(mlink, (float*)data, dims, heads, actualDepth);
            break;
        case TYPE_DOUBLE:
            MLReleaseReal64Array(mlink, (double*)data, dims, heads, actualDepth);
            break;
    }

    // Py_DECREF(headsArray);

    return retval;
}

/* Never called unless we already know that the JNI native size of the primitive array can be handled by an MLPutXXXArray
   raw MathLink call. Thus, we never have to worry about manually sending an array element-by-element.
*/
/*MLPutArrayFlat
JNIEXPORT void MLFUNC(MLPutArrayFlat), jlong link, jint type, jobject data, jobjectArray heads, jintArray dims) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    int depth;
    jint *temp_dims;
    int c_dims[5];
    char head1[256], head2[256], head3[256], head4[256], head5[256];
    char *c_heads[5];
    int usesHeads;
    int i;

    c_heads[0] = head1;
    c_heads[1] = head2;
    c_heads[2] = head3;
    c_heads[3] = head4;
    c_heads[4] = head5;

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutArrayFlat")
        return;
    } else if (data == NULL) {
        DEBUGSTR1(" data is null in MLPutArrayFlat")
        return;
    }

    depth = (*env)->GetArrayLength(env, dims);
    temp_dims = (*env)->GetIntArrayElements(env, dims, NULL);
    if (temp_dims == NULL) {
        DEBUGSTR1(" mem failure in MLPutArrayFlat")
        return;
    }
    for (i = 0; i < depth && i < 5; i++)
        c_dims[i] = temp_dims[i];  // Converting from jint to long.
    (*env)->ReleaseIntArrayElements(env, dims, temp_dims, JNI_ABORT);

    usesHeads = heads != (jobjectArray) NULL;
    if (usesHeads) {
        for (i = 0; i < (*env)->GetArrayLength(env, heads); i++) {
            jstring s = (jstring)((*env)->GetObjectArrayElement(env, heads, i));
            char *p = (char*) (*env)->GetStringUTFChars(env, s, NULL);
            strncpy(c_heads[i], p, 255);
            c_heads[i][255] = 0;
            (*env)->ReleaseStringUTFChars(env, s, p);
        }
    }

    switch (type) {
        case TYPE_BYTE:  // Byte was converted to short in Java code, because MLPutByteArray won't work--it converts to unsigned.
        case TYPE_SHORT: {
            jshort *c_data = (*env)->GetShortArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArrayFlat")
                break;
            }
            MLPutInteger16Array(mlink, (short*)c_data, c_dims, usesHeads ? c_heads : NULL, depth);
            (*env)->ReleaseShortArrayElements(env, data, c_data, JNI_ABORT);
            break;
        }
        case TYPE_CHAR:  // Char was converted to int in Java code, because MLPutShortIntegerArray won't work--it converts to signed.
        case TYPE_INT: {
            jint *c_data = (*env)->GetIntArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArrayFlat")
                break;
            }
            MLPutInteger32Array(mlink, (int*)c_data, c_dims, usesHeads ? c_heads : NULL, depth);
            (*env)->ReleaseIntArrayElements(env, data, c_data, JNI_ABORT);
            break;
        }
        case TYPE_LONG: {
            jlong *c_data = (*env)->GetLongArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                break;
            }
            MLPutInteger64Array(mlink, (mlint64*)c_data, c_dims, usesHeads ? c_heads : NULL, depth);
            (*env)->ReleaseLongArrayElements(env, data, c_data, JNI_ABORT);
            break;
        }
        case TYPE_FLOAT: {
            jfloat *c_data = (*env)->GetFloatArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArrayFlat")
                break;
            }
            MLPutReal32Array(mlink, (float*)c_data, c_dims, usesHeads ? c_heads : NULL, depth);
            (*env)->ReleaseFloatArrayElements(env, data, c_data, JNI_ABORT);
            break;
        }
        case TYPE_DOUBLE: {
            jdouble *c_data = (*env)->GetDoubleArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArrayFlat")
                break;
            }
            MLPutReal64Array(mlink, (double*)c_data, c_dims, usesHeads ? c_heads : NULL, depth);
            (*env)->ReleaseDoubleArrayElements(env, data, c_data, JNI_ABORT);
            break;
        }
        default:
            break;
    }
    return;
}*/

MLFUNCWITHARGS(MLPutArrayFlat){
    _MLDebugMessage(2, ":PutArrayFlat:");

    // jlong link, jint type, jobject data, jobjectArray heads, jintArray dims

    PyObject *ml, *data, *heads, *dims;
    int type;
    MLPARSEARGS("niOOO", &ml, &type, &data, &heads, &dims);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    PyObject *iterator = PyObject_GetIter(dims);
    PyObject *item;

    int c_dims[9];
    int depth = 0;
    if (iterator == NULL) {
        // Py_DECREF(data);
        // Py_DECREF(heads);
        // Py_DECREF(dims);
        return NULL;
    }
    while (depth < 9 && ( item = PyIter_Next(iterator) )) {
        c_dims[depth] = (int) PyLong_AsLong(item);
        depth++;
        Py_DECREF(item);
    }
    Py_DECREF(iterator);
    // Py_DECREF(dims);

    MLCHECKERROR();

    char head1[256], \
            head2[256], \
            head3[256], \
            head4[256], \
            head5[256], \
            head6[256], \
            head7[256], \
            head8[256], \
            head9[256];
    char *c_heads[9];
    int usesHeads;
    int i;

    c_heads[0] = head1;
    c_heads[1] = head2;
    c_heads[2] = head3;
    c_heads[3] = head4;
    c_heads[4] = head5;
    c_heads[5] = head6;
    c_heads[6] = head7;
    c_heads[7] = head8;
    c_heads[8] = head9;

    if (mlink == 0) {
        return NULL;
    } else if (data == NULL) {
        return NULL;
    }

    usesHeads = PySequence_Size(heads);
    if (usesHeads) {
        for (i = 0; i < usesHeads; i++) {
            PyObject *s = PySequence_GetItem(heads, i);
            const char *p = _MLGetString(s);
            strncpy(c_heads[i], p, 255);
            c_heads[i][255] = 0;
        }
    }
    // Py_DECREF(heads);

    int flag = 1;
    const char ** headList = const_cast<const char**> (usesHeads ? c_heads : NULL);
    switch (type) {
        case TYPE_BYTE:  // Byte was converted to short in Java code, because MLPutByteArray won't work--it converts to unsigned.
                         // Unclear to me what ^this means on the python side...
        case TYPE_SHORT: {
            flag = _MLPutDataBuffer(
                        mlink, data, 'i', 16, c_dims, headList, depth
                        );
            break;
        }
        case TYPE_CHAR:  // Char was converted to int in Java code, because MLPutShortIntegerArray won't work--it converts to signed.
                         // Unclear to me what ^this means on the python side...
        case TYPE_INT: {
            flag = _MLPutDataBuffer(
                        mlink, data, 'i', 32, c_dims, headList, depth
                        );
            break;
        }
        case TYPE_LONG: {
            flag = _MLPutDataBuffer(
                        mlink, data, 'i', 64, c_dims, headList, depth
                        );
            break;
        }
        case TYPE_FLOAT: {
            flag = _MLPutDataBuffer(
                        mlink, data, 'r', 32, c_dims, headList, depth
                        );
            break;
        }
        case TYPE_DOUBLE: {
            flag = _MLPutDataBuffer(
                        mlink, data, 'r', 64, c_dims, headList, depth
                        );
            break;
        }
        default:
            break;
    }

    // Py_DECREF(data);

    if (!flag) return NULL;
    Py_RETURN_NONE;

}

/*MLPutArray
JNIEXPORT void MLFUNC(MLPutArray), jlong link, jint type, jobject data, jstring head) {

    // Changed in J/Link 1.1. Now it is only passed 1-D arrays. Slicing of arrays is now done in Java code.

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    int i;
    char c_head[256]; // Limit of 256 chars on symbol names for head at deepest level of array.
    char *head_ptr = c_head;
    int len;

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutArray")
        return;
    } else if (data == NULL) {
        DEBUGSTR1(" data is null in MLPutArray")
        return;
    }

    if (head != NULL) {
        char *p = (char*) (*env)->GetStringUTFChars(env, head, NULL);
        strncpy(c_head, p == NULL ? "List" : p, 255);
        c_head[255] = 0;
        (*env)->ReleaseStringUTFChars(env, head, p);
    } else {
        c_head[0] = 'L'; c_head[1] = 'i'; c_head[2] = 's'; c_head[3] = 't'; c_head[4] = 0;
    }

    len = (*env)->GetArrayLength(env, data);
    if (len == 0) {
        // MLPutXXXArray functions in the MathLink C API cannot handle arrays of length 0, so we do these
        // differently. All arrays with a 0 anywhere in their dims are routed in Java so that they will go through here
        // (never MLPutArrayFlat()).
        MLPutFunction(mlink, c_head, 0);
        return;
    }
    switch (type) {
        case TYPE_BOOLEAN: {
            jboolean *c_data = (*env)->GetBooleanArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            MLPutFunction(mlink, c_head, len);
            for (i = 0; i < len; i++) {
                MLPutSymbol(mlink, c_data[i] ? "True" : "False");
            }
            (*env)->ReleaseBooleanArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_BYTE: {
            short* shortData, *p;
            jbyte* jp;
            jbyte *c_data = (*env)->GetByteArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            // If we use MLPutByteArray, we get conversion to 0..255 range. Thus, we use alternate means.
            shortData = (short*) malloc(len * sizeof(short));
            if (shortData != NULL) {
                for (p = shortData, jp = c_data; p - shortData < len; )
                    *p++ = *jp++;
                MLPutInteger16Array(mlink, shortData, &len, &head_ptr, 1);
                free(shortData);
            } else {
                MLPutFunction(mlink, c_head, len);
                for (i = 0; i < len; i++) {
                    MLPutInteger(mlink, (int) c_data[i]);
                }
            }
            (*env)->ReleaseByteArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_CHAR: {
            jchar *c_data = (*env)->GetCharArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            // If we use MLPutShortIntegerArray, we get conversion to -32768..32767 range. Thus, we put the array piecemeal.
            // I could try the trick used above for byte, but that would cost more memory for a modest speedup. Not too concerned
            // about speed with the rare type char.
            MLPutFunction(mlink, c_head, len);
            for (i = 0; i < len; i++) {
                MLPutInteger(mlink, (int) c_data[i]);
            }
            (*env)->ReleaseCharArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_SHORT: {
            jshort *c_data = (*env)->GetShortArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            MLPutInteger16Array(mlink, (short*)c_data, &len, &head_ptr, 1);
            (*env)->ReleaseShortArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_INT: {
            jint *c_data = (*env)->GetIntArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            MLPutInteger32Array(mlink, (int*)c_data, &len, &head_ptr, 1);
            (*env)->ReleaseIntArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_LONG: {
            jlong *c_data = (*env)->GetLongArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            MLPutInteger64Array(mlink, (mlint64*)c_data, &len, &head_ptr, 1);
            (*env)->ReleaseLongArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_FLOAT: {
            jfloat *c_data = (*env)->GetFloatArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            MLPutReal32Array(mlink, (float*)c_data, &len, &head_ptr, 1);
            (*env)->ReleaseFloatArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_DOUBLE: {
            jdouble *c_data = (*env)->GetDoubleArrayElements(env, data, NULL);
            if (c_data == NULL) {
                DEBUGSTR1(" mem failure in MLPutArray")
                return;
            }
            MLPutReal64Array(mlink, (double*)c_data, &len, &head_ptr, 1);
            (*env)->ReleaseDoubleArrayElements(env, data, c_data, JNI_ABORT);
            return;
        }
        case TYPE_STRING: {
            MLPutFunction(mlink, c_head, len);
            for (i = 0; i < len; i++) {
                jstring s = (jstring)((*env)->GetObjectArrayElement(env, data, i));
                if (s != NULL) {
                    const jchar* chars = (*env)->GetStringChars(env, s, NULL);
                    MLPutUCS2String(mlink, chars, (*env)->GetStringLength(env, s));
                    (*env)->ReleaseStringChars(env, s, chars);
                    (*env)->DeleteLocalRef(env, s);
                } else {
                    MLPutSymbol(mlink, "Null");
                }
            }
            return;
        }
        default:
            DEBUGSTR1(" Bad type in MLPutArray")
            break;
    }
}*/

MLFUNCWITHARGS(MLPutArray) {
    _MLDebugMessage(2, ":PutArray:");

    //jlong link, jint type, jobject data, jstring head
    // Changed in J/Link 1.1. Now it is only passed 1-D arrays. Slicing of arrays is now done in Java code.

    PyObject *ml, *data, *head;
    int type;
    MLPARSEARGS("OiOOO", &ml, &type, &data, &head);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    // int i;
    char c_head[256]; // Limit of 256 chars on symbol names for head at deepest level of array.
    const char *head_ptr = c_head;
    int len;

    if (mlink != 0) {

        if ( head != Py_None ) {
            const char *p = _MLGetString(head);
            strncpy(c_head, p == NULL ? "List" : p, 255);
            c_head[255] = 0;
            // Py_DECREF(head);
        } else {
            c_head[0] = 'L'; c_head[1] = 'i'; c_head[2] = 's'; c_head[3] = 't'; c_head[4] = 0;
        }

        len = PySequence_Length(data);
        if (len == 0) {
            // MLPutXXXArray functions in the MathLink C API cannot handle arrays of length 0, so we do these
            // differently. All arrays with a 0 anywhere in their dims are routed in Java so that they will go through here
            // (never MLPutArrayFlat()).
            MLPutFunction(mlink, c_head, 0);
        } else {
            switch (type) {
                case TYPE_BOOLEAN: {
                    _MLIterPut(mlink, data, c_head, type);
                    break;
                }
                case TYPE_BYTE: {
                    if (!_MLPutDataBuffer(mlink, data, 'b', 16, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_CHAR: {
                    if (!_MLPutDataBuffer(mlink, data, 'i', 0, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_SHORT: {
                    if (!_MLPutDataBuffer(mlink, data, 'i', 16, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_INT: {
                    if (!_MLPutDataBuffer(mlink, data, 'i', 32, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_LONG: {
                    if (!_MLPutDataBuffer(mlink, data, 'i', 64, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_FLOAT: {
                    if (!_MLPutDataBuffer(mlink, data, 'f', 32, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_DOUBLE: {
                    if (!_MLPutDataBuffer(mlink, data, 'f', 64, &len, &head_ptr, 1)) {
                        _MLIterPut(mlink, data, c_head, type);
                    }
                    break;
                }
                case TYPE_STRING: {
                   _MLIterPut(mlink, data, c_head, type);
                   break;
                }
                default:
                    break;
            }
        }
    }

    // Py_DECREF(data);
    // Py_DECREF(head);
    Py_RETURN_NONE;
}

/*JAVA ONLY nativeSizesMatch
JNIEXPORT jboolean MLFUNC(nativeSizesMatch), jint type) {

    switch (type) {
        case TYPE_BYTE:  // byte arrays are are expanded to short in the Java code, so the test is the same.
        case TYPE_SHORT:
            return (jboolean) (sizeof(jshort) == sizeof(short));
        case TYPE_CHAR:  // char arrays are are expanded to int in the Java code, so the test is the same.
        case TYPE_INT:
            return (jboolean) (sizeof(jint) == sizeof(int) || sizeof(jint) == sizeof(long));
        case TYPE_LONG:
            return (jboolean) (sizeof(jlong) == sizeof(long) || sizeof(jlong) == sizeof(int));
        case TYPE_FLOAT:
            return (jboolean) (sizeof(jfloat) == sizeof(float));
        case TYPE_DOUBLE:
            return (jboolean) (sizeof(jdouble) == sizeof(double) || sizeof(jdouble) == sizeof(float));
        default:
            return (jboolean) 0; // Satisfy the compiler only; code should never reach here.
    }
}*/

/*MLTransferExpression
JNIEXPORT void MLFUNC(MLTransferExpression), jlong dest, jlong source) {

    if (PTR_FROM_JLONG(source) == 0 || PTR_FROM_JLONG(dest) == 0) {
        DEBUGSTR1(" link or source is null in MLTransferExpression")
    } else {
        MLTransferExpression((MLINK) PTR_FROM_JLONG(dest), (MLINK) PTR_FROM_JLONG(source));
    }
}*/

MLFUNCWITHARGS(MLTransferExpression) {
    _MLDebugMessage(2, ":TransferExpression:");
    //jlong dest, jlong source

    int64_t ldest, lsource;
    MLPARSEARGS("nn", &ldest, &lsource);
    MLINK dest = (MLINK) ldest;
    MLINK source = (MLINK) ldest;


    if (dest != 0 && source != 0) {
        MLTransferExpression(dest, source);
    };

    Py_RETURN_NONE;

}

/*MLTransferToEndOfLoopbackLink
JNIEXPORT void MLFUNC(MLTransferToEndOfLoopbackLink), jlong dest, jlong source) {

    if (PTR_FROM_JLONG(source) == 0 || PTR_FROM_JLONG(dest) == 0) {
        DEBUGSTR1(" link or source is null in MLTransferToEndOfLoopbackLink")
    } else {
        MLTransferToEndOfLoopbackLink((MLINK) PTR_FROM_JLONG(dest), (MLINK) PTR_FROM_JLONG(source));
    }
}*/

MLFUNCWITHARGS(MLTransferToEndOfLoopbackLink) {
    _MLDebugMessage(2, ":TransferToEndOfLoopbackLink:");
    //jlong dest, jlong source

    int64_t ldest, lsource;
    MLPARSEARGS("nn", &ldest, &lsource);
    MLINK dest = (MLINK) ldest;
    MLINK source = (MLINK) ldest;

    if (dest != 0 && source != 0) {
        MLTransferToEndOfLoopbackLink(dest, source);
    }

    Py_RETURN_NONE;

}

/*MLGetMessage
JNIEXPORT jint MLFUNC(MLGetMessage), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLGetMessage")
        return (jint) 0;
    } else {
        int m1 = 0, m2 = 0;
        MLGetMessage(mlink, &m1, &m2);
        return (jint) m1;
    }
}*/

MLFUNCWITHARGS(MLGetMessage) {
    _MLDebugMessage(2, ":GetMessage:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink == 0) {
        MLRETURNINT(0);
    } else {
        int m1 = 0, m2 = 0;
        MLGetMessage(mlink, &m1, &m2);
        MLRETURNINT(m1);
    }

}

/*MLPutMessage
JNIEXPORT void MLFUNC(MLPutMessage), jlong link, jint msg) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLPutMessage")
    } else {
        MLPutMessage(mlink, (int) msg);
    }
}*/

MLFUNCWITHARGS(MLPutMessage) {
    _MLDebugMessage(2, ":PutMessage:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    int msg;
    MLPARSEARGS("Oi", &ml, &msg);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    if (mlink != 0) {
        MLPutMessage(mlink, msg);
    }

    Py_RETURN_NONE;

}

/*MLMessageReady
JNIEXPORT jboolean MLFUNC(MLMessageReady), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLMessageReady")
        return (jboolean) 0;
    } else {
        return (jboolean) MLMessageReady(mlink);
    }
}*/

MLFUNCWITHARGS(MLMessageReady) {
    _MLDebugMessage(2, ":MessageReady:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink == 0) {
        MLRETURNBOOL(0);
    } else {
        MLRETURNBOOL(MLMessageReady(mlink));
    }

}

/*MLCreateMark
JNIEXPORT jlong MLFUNC(MLCreateMark), jlong link) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLCreateMark")
                return PYINT_FROM_PTR(0);
    } else {
                return PYINT_FROM_PTR(MLCreateMark(mlink));
    }
}*/

MLFUNCWITHARGS(MLCreateMark) {
    _MLDebugMessage(2, ":CreateMark:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    MLPARSEARGS("O", &ml);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);


    if (mlink == 0) {
        MLRETURNPTR(0);
    } else {
        MLRETURNPTR(MLCreateMark(mlink));
    }

}

/*MLSeekMark
JNIEXPORT void MLFUNC(MLSeekMark), jlong link, jlong mark) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLSeekMark")
    } else {
        MLSeekMark(mlink, (MLINKMark) PTR_FROM_JLONG(mark), 0);
    }
}*/

MLFUNCWITHARGS(MLSeekMark) {
    _MLDebugMessage(2, ":SeekMark:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    long mark;
    MLPARSEARGS("Ol", &ml, &mark);
    MLINK mlink = _MLGetMLINK(ml);


    if (mlink != 0) {
        MLSeekMark(mlink, (MLINKMark) mark, 0);
    }

    Py_RETURN_NONE;

}

/*MLDestroyMark
JNIEXPORT void MLFUNC(MLDestroyMark), jlong link, jlong mark) {

    MLINK mlink = (MLINK) PTR_FROM_JLONG(link);

    if (mlink == 0) {
        DEBUGSTR1(" link is 0 in MLDestroyMark")
    } else {
        MLDestroyMark(mlink, (MLINKMark) PTR_FROM_JLONG(mark));
    }
}*/

MLFUNCWITHARGS(MLDestroyMark) {
    _MLDebugMessage(2, ":DestroyMark:");

    // Basic structure o be used whenever a link is needed
    PyObject *ml;
    long mark;
    MLPARSEARGS("Ol", &ml, &mark);
    MLINK mlink = _MLGetMLINK(ml);

    if (mlink != 0) {
        MLDestroyMark(mlink, (MLINKMark) mark);
    }

    Py_RETURN_NONE;

}

/***************************  Environment init/deinit  *****************************/

static int initEnvironment(void) {

    MLEnvironmentParameter p;
    if (gMLEnv == (MLEnvironment) 0) {
#ifdef WINDOWS_MATHLINK
        gMLEnv = MLBegin(0);
#else
        p = MLNewParameters(MLREVISION, MLAPIREVISION);
        /* Prevent collision of MathLink's SIGSEGV handler with the one Java installs. See bug 94033. */
        MLDoNotHandleSignalParameter(p, SIGSEGV);
        gMLEnv = MLBegin(p);
#endif
    }
    return gMLEnv != (MLEnvironment) 0;
}

static void destroyEnvironment(void) {

    if (gMLEnv != (MLEnvironment) 0) {
        MLEnd(gMLEnv);
        gMLEnv = (MLEnvironment) 0;
    }
}

/****************************  Yielding and Messages  *********************************/

static void setupUserData(MLINK mlp, MLEnvironment mlEnv, PyObject *ml) {

    MLYieldFunctionObject yielder = MLCreateYieldFunction(mlEnv, (MLYieldFunctionType) yield_func, 0);
    MLMessageHandlerObject handler = MLCreateMessageHandler(mlEnv, (MLMessageHandlerType) msg_handler, 0);
    MLSetYieldFunction(mlp, yielder);
    MLSetMessageHandler(mlp, handler);

    PyObject *msgMethod = PyObject_GetAttrString(ml, "nativeMessageCallback");
    if ( msgMethod == NULL ) return;
    PyObject *yieldMethod = PyObject_GetAttrString(ml, "nativeYielderCallback");
    if ( yieldMethod == NULL ) {
        // Py_DECREF(msgMethod);
        return;
    };

    //    MLYieldFunctionObject yielder;
    //    MLMessageHandlerObject msgHandler;
    //    PyObject *ml;
    //    PyObject *yieldFunction;
    //    PyObject *messageHandler;
    //    int usePythonYielder;
    //    int usePythonMsgHandler;

    struct cookie cookie = {
        // MLYieldFunctionObject yielder ->
        yielder,
                // MLMessageHandlerObject msgHandler ->
                handler,
                // PyObject *ml ->
                ml,
                // PyObject *yieldFunction ->
                yieldMethod,
                // PyObject *messageHandler ->
                msgMethod,
                // int useNumPy ->
                0,
                // int usePythonYielder ->
                0,
                // int usePythonMsgHandler ->
                0
    };

    MLSetUserData(mlp, (char *) &cookie, NULL);
}

/*setupCallback
void setupCallback(jlong link, CallbackType type, int revoke) {

    MLINK mlp = PTR_FROM_JLONG(link);
    struct cookie* cookie;

    if (mlp == 0) {
        DEBUGSTR1(" link is 0 in setupCallback")
        return;
    }

    cookie = (struct cookie*) MLUserData(mlp, NULL);

    if (cookie == NULL) {
        // Should never happen. Would require a user to attempt this operation on a loopback link.
        DEBUGSTR1(" cookie is NULL in setupCallback")
        return;
    }

    if (type == kYield)
        cookie->useJavaYielder = revoke ? 0 : 1;
    else
        cookie->useJavaMsgHandler = revoke ? 0 : 1;
}*/

void setupCallback(PyObject *ml, CallbackType type, int revoke) {

    MLINK mlp = _MLGetMLINK(ml);

    if (mlp == 0) {
        return;
    }

    struct cookie* cookie;
    cookie = (struct cookie*) MLUserData(mlp, NULL);

    if (cookie == NULL) {
        // Should never happen. Would require a user to attempt this operation on a loopback link.
        return;
    }

    if (type == kYield)
        cookie->usePythonYielder = revoke ? 0 : 1;
    else
        cookie->usePythonMsgHandler = revoke ? 0 : 1;
}

/*MLMDEFN
MLMDEFN(void, msg_handler, (MLINK mlp, int message, int n)) {

    int needsAttach = 1;
    JNIEnv* env;
    struct cookie* cookie = (struct cookie*) MLUserData(mlp, NULL);
    if (cookie == NULL) {
        // Should never happen.
        DEBUGSTR1("cookie was NULL in msg_handler")
        return;
    }

    if (message == MLTerminateMessage)
        gWasTerminated = 1;

    if (!cookie->useJavaMsgHandler) {
        // Normal. No Java-side msghandler set.
        return;
    }

    needsAttach = (*(cookie->jvm))->GetEnv(cookie->jvm, (void**) &env, JNI_VERSION_1_2) == JNI_EDETACHED;

    if (needsAttach) {
        if ((*(cookie->jvm))->AttachCurrentThread(cookie->jvm, (void**) &env, NULL) != 0) {
            DEBUGSTR1("AttachCurrentThread in msghandler failed")
            return;
        }
    }

    // The function being called here is nativeMessageCallback in class MathLink.
    (*env)->CallVoidMethod(env, cookie->ml, cookie->messageHandler, (jint) message, (jint) n);

    if (needsAttach)
        (*(cookie->jvm))->DetachCurrentThread(cookie->jvm);
}*/

MLMDEFN(void, msg_handler, (MLINK mlp, int message, int n)) {

    // int needsAttach = 1; // don't know how to work with this...........???
    struct cookie* cookie = (struct cookie*) MLUserData(mlp, NULL);
    if (cookie == NULL) {
        // Should never happen.
        // DEBUGSTR1("cookie was NULL in msg_handler")
        return;
    }

    if (message == MLTerminateMessage)
        gWasTerminated = 1;

    if (!cookie->usePythonMsgHandler) {
        // Normal. No Java-side msghandler set.
        return;
    }

//    needsAttach = (*(cookie->jvm))->GetEnv(cookie->jvm, (void**) &env, JNI_VERSION_1_2) == JNI_EDETACHED;

//    if (needsAttach) {
//        if ((*(cookie->jvm))->AttachCurrentThread(cookie->jvm, (void**) &env, NULL) != 0) {
//            DEBUGSTR1("AttachCurrentThread in msghandler failed")
//            return;
//        }
//    }

//    // The function being called here is nativeMessageCallback in class MathLink.
//    (*env)->CallVoidMethod(env, cookie->ml, cookie->messageHandler, (jint) message, (jint) n);

    PyObject *args = Py_BuildValue("(ii)", message, n);
    PyObject *kwargs = PyDict_New();
    PyObject *pyres = PyObject_Call(cookie->messageHandler, args, kwargs);
    Py_DECREF(args);
    Py_DECREF(kwargs);
    Py_DECREF(pyres);

//    if (needsAttach)
//        (*(cookie->jvm))->DetachCurrentThread(cookie->jvm);

}

/*MLYDEFN
MLYDEFN(devyield_result, yield_func, (MLINK mlp, MLYieldParameters yp)) {

    int needsAttach = 1;
    jboolean res = 0;
    struct cookie* cookie = (struct cookie*) MLUserData(mlp, NULL);
    JNIEnv* env;

    // If an MLTerminateMessage has arrived, immediately back out of any read calls.
    if (gWasTerminated)
        return 1;

    if (cookie == NULL) {
        // Should never happen.
        DEBUGSTR1("cookie was NULL in yieldfunction")
        return 0;
    }

    if (!cookie->useJavaYielder) {
        // Normal. No Java-side yielder set.
        return 0;
    }

    needsAttach = (*(cookie->jvm))->GetEnv(cookie->jvm, (void**) &env, JNI_VERSION_1_2) == JNI_EDETACHED;

    if (needsAttach) {
        if ((*(cookie->jvm))->AttachCurrentThread(cookie->jvm, (void**) &env, NULL) != 0) {
            DEBUGSTR1("AttachCurrentThread in yieldfunction failed")
            return 0;
        }
    }

    // The function being called here is nativeYielderCallback in class MathLink.
    res = (*env)->CallBooleanMethod(env, cookie->ml, cookie->yieldFunction, 0 // will be removed);

    if (needsAttach)
        (*(cookie->jvm))->DetachCurrentThread(cookie->jvm);

    return (devyield_result) res;
}*/

MLYDEFN(devyield_result, yield_func, (MLINK mlp, MLYieldParameters yp)) {

     // int needsAttach = 1;
    struct cookie* cookie = (struct cookie*) MLUserData(mlp, NULL);

    // If an MLTerminateMessage has arrived, immediately back out of any read calls.
    if (gWasTerminated)
        return 1;

    if (cookie == NULL) {
        // Should never happen.
        return 0;
    }

    if (!cookie->usePythonYielder) {
        // Normal. No Java-side yielder set.
        return 0;
    }

//    needsAttach = (*(cookie->jvm))->GetEnv(cookie->jvm, (void**) &env, JNI_VERSION_1_2) == JNI_EDETACHED;

//    if (needsAttach) {
//        if ((*(cookie->jvm))->AttachCurrentThread(cookie->jvm, (void**) &env, NULL) != 0) {
//            DEBUGSTR1("AttachCurrentThread in yieldfunction failed")
//            return 0;
//        }
//    }

//    // The function being called here is nativeYielderCallback in class MathLink.
//    res = (*env)->CallBooleanMethod(env, cookie->ml, cookie->yieldFunction, 0 // will be removed);

      PyObject *args = Py_BuildValue("(i)", 0);
      PyObject *kwargs = PyDict_New();
      PyObject *pyres = PyObject_Call(cookie->yieldFunction, args, kwargs);
      Py_DECREF(args);
      Py_DECREF(kwargs);

      long res = PyLong_AsLong(pyres);
      Py_DECREF(pyres);

//    if (needsAttach)
//        (*(cookie->jvm))->DetachCurrentThread(cookie->jvm);

    return (devyield_result) res;
}

/********************************* ARRAY TYPES ***************************************/

/*static jobject MakeArray1(int type, int len, const void* startAddr) {

    int i;
    switch (type) {
        case TYPE_BYTE: {
            jbyte* a;
            jbyteArray ja = (*env)->NewByteArray(env, len);
            if (ja == NULL) {
                return (jobject) NULL;
            }
            // Cannot use SetByteArrayRegion because we have a short*, not a char*.
            a = (*env)->GetByteArrayElements(env, ja, NULL);
            if (a == NULL) {
                DEBUGSTR1(" GetByteArrayElements failed in makeArray1");
                return (jobject) NULL;
            }
            for (i = 0; i < len; i++) {
                a[i] = (jbyte) ((short*) startAddr)[i];
            }
            (*env)->ReleaseByteArrayElements(env, ja, a, 0);
            return (jobject) ja;
        }
        case TYPE_CHAR: {
            jchar* a;
            jcharArray ja = (*env)->NewCharArray(env, len);
            if (ja == NULL) {
                return (jobject) NULL;
            }
            // Cannot use SetCharArrayRegion because we have an int*, not a short*.
            a = (*env)->GetCharArrayElements(env, ja, NULL);
            if (a == NULL) {
                DEBUGSTR1(" GetCharArrayElements failed in makeArray1");
                return (jobject) NULL;
            }
            for (i = 0; i < len; i++) {
                a[i] = (jchar) ((int*) startAddr)[i];
            }
            (*env)->ReleaseCharArrayElements(env, ja, a, 0);
            return (jobject) ja;
        }
        case TYPE_SHORT: {
            jshortArray ja = (*env)->NewShortArray(env, len);
            if (ja == NULL) {
                return (jobject) NULL;
            }
            (*env)->SetShortArrayRegion(env, ja, 0, len, (jshort*)startAddr);
            return (jobject) ja;
        }
        case TYPE_INT: {
            jintArray ja = (*env)->NewIntArray(env, len);
            if (ja == NULL) {
                return (jobject) NULL;
            }
            (*env)->SetIntArrayRegion(env, ja, 0, len, (jint*)startAddr);
            return (jobject) ja;
        }
        case TYPE_FLOAT: {
            jfloatArray ja = (*env)->NewFloatArray(env, len);
            if (ja == NULL) {
                return (jobject) NULL;
            }
            (*env)->SetFloatArrayRegion(env, ja, 0, len, (jfloat*)startAddr);
            return (jobject) ja;
        }
        case TYPE_DOUBLE: {
            jdoubleArray ja = (*env)->NewDoubleArray(env, len);
            if (ja == NULL) {
                return (jobject) NULL;
            }
            (*env)->SetDoubleArrayRegion(env, ja, 0, len, (jdouble*)startAddr);
            return (jobject) ja;
        }
        default:
            return NULL;  // Just to silence a compiler warning.
    }

}*/

static PyObject *MakeArray1(int type, int len, const void* startAddr) {

    // int i;
    switch (type) {
        case TYPE_BYTE: {
            PyObject *array = PyByteArray_FromStringAndSize( (const char *) startAddr, len);
            if ( array == NULL )
                return NULL;
            return array;
        }
        case TYPE_CHAR: {
            PyObject *array = _MLMakeNewArray(type, len);
            if ( array == NULL )
                return NULL;
            _MLPopulateArray(array, startAddr, len*_MLDataSize(type));
            return array;
        }
        case TYPE_SHORT: {
            PyObject *array = _MLMakeNewArray(type, len);
            if ( array == NULL )
                return NULL;
            _MLPopulateArray(array, startAddr, len*_MLDataSize(type));
            return array;
        }
        case TYPE_INT: {
            PyObject *array = _MLMakeNewArray(type, len);
            if ( array == NULL )
                return NULL;
            _MLPopulateArray(array, startAddr, len*_MLDataSize(type));
            return array;
        }
        case TYPE_FLOAT: {
            PyObject *array = _MLMakeNewArray(type, len);
            if ( array == NULL )
                return NULL;
            _MLPopulateArray(array, startAddr, len*_MLDataSize(type));
            return array;
        }
        case TYPE_DOUBLE: {
            PyObject *array = _MLMakeNewArray(type, len);
            if ( array == NULL )
                return NULL;
            _MLPopulateArray(array, startAddr, len*_MLDataSize(type));
            return array;
        }
        default:
            return NULL;  // Just to silence a compiler warning.
    }
}

// should add compiler logic for if NumPy is to be used
static PyObject *MakeArrayN(int type, int depth, int* dims, int curDepth, int lenInFinalDim, const void* startAddr) {

    PyObject *array;
    int i, datSize;
    char typeChar;

    if (depth == 1)
        return MakeArray1(type, lenInFinalDim, startAddr);

    datSize=_MLDataSize(type);
    typeChar=_MLTypeChar(type);

    array=PyList_New(depth);

    if (array == NULL) {
        return NULL;
    }
    for (i = 0; i < dims[curDepth]; i++) {
        PyObject *array2;
        size_t jump = lenInFinalDim;
        int j;
        for (j = curDepth + 1; j < curDepth + depth - 1; j++)
            jump *= dims[j];
        array2 = MakeArrayN(type, depth - 1, dims, curDepth + 1, lenInFinalDim, (char*)startAddr + i*jump*datSize);
        if (array2 == NULL) {
            return NULL;
        }
        PyList_SetItem(array, i, array2);
    }
    return array;
}

/****************************  platform-dependent  *********************************/

/* Bit of a hack. Want to minimize Java MS-DOS window after Java is launched. */

/*JAVA ONLY hideJavaWindow
JNIEXPORT void MLFUNC(hideJavaWindow)) {

#ifdef WINDOWS_MATHLINK
    BOOL CALLBACK hideJavaWindowCallback(HWND, LPARAM);
    WNDENUMPROC callbackFunc = (WNDENUMPROC) &hideJavaWindowCallback;
    EnumWindows(callbackFunc, 0);
#endif
}

#ifdef WINDOWS_MATHLINK

BOOL CALLBACK hideJavaWindowCallback(HWND hwnd, LPARAM lParam) {
    DWORD winProcID, curProcID;
    curProcID = GetCurrentProcessId();
    GetWindowThreadProcessId(hwnd, &winProcID);
    if (curProcID == winProcID && IsWindowVisible(hwnd))
        CloseWindow(hwnd);
    return TRUE;
}

#endif*/

/* Need a little help from C for Mac and Windows to get Java windows to the foreground. */

/*JAVA ONLY macJavaLayerToFront
JNIEXPORT void MLFUNC(macJavaLayerToFront)) {

#if defined(DARWIN_MATHLINK) || defined(X86_DARWIN_MATHLINK) || defined(X86_64_DARWIN_MATHLINK)
    ProcessSerialNumber psn;
    if (GetCurrentProcess(&psn) == noErr)
        SetFrontProcess(&psn);
#endif
}*/

/*winJavaLayerToFront
JNIEXPORT void MLFUNC(winJavaLayerToFront), jboolean attach) {

#ifdef WINDOWS_MATHLINK
    // We call AttachThreadInput to get around restrictions in Win 98 and later on who is allowed to set the
    // foreground window. Java can't put its window to the front since it is not the foreground app, but this
    // trick lets us pretend that our thread is the foreground thread. The attach param is true when this is
    // called before toFront() and false when it is called after.
    DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD thisThread = GetCurrentThreadId();
    if (foregroundThread != thisThread)
        AttachThreadInput(foregroundThread, thisThread, attach);
#endif
}*/

// Only for OSX, puts Mathematica back in foreground after JLink app launches.
/*mathematicaToFront
JNIEXPORT void MLFUNC(mathematicaToFront)) {

#if defined(DARWIN_MATHLINK) || defined(X86_DARWIN_MATHLINK) || defined(X86_64_DARWIN_MATHLINK)
    ProcessSerialNumber psn;
    ProcessInfoRec infoRec;
    unsigned char name[256];
    infoRec.processInfoLength = sizeof(ProcessInfoRec);
    // ProcessInfoRec struct differs in 64-bit version of OSX.
#if __LP64__
    infoRec.processAppRef = NULL;
#else
    infoRec.processAppSpec = NULL;
#endif
    infoRec.processName = name;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        if (GetProcessInformation(&psn, &infoRec) == noErr) {
            if (infoRec.processType == 'APPL' && infoRec.processSignature == 'OMEG') {
                SetFrontProcess(&psn);
                return;
            }
        }
    }
#endif
}*/

/*JAVA ONLY getNativeWindowHandle
JNIEXPORT jlong MLFUNC(getNativeWindowHandle), jobject windowObj, jstring javaHomePath) {

#if defined(WINDOWS_MATHLINK)
    JAWT awt;
    JAWT_DrawingSurface* ds;
    JAWT_DrawingSurfaceInfo* dsi;
    JAWT_Win32DrawingSurfaceInfo* dsi_win;
    jint lock;
    jlong hwnd = -1;

    // Dynamically load jawt.dll and find the JAWT_GetAWT entry point.
    // Bynamic loading instead of linking against jawt.lib avoids fatal
    // errors that have occurred in certain Java configurations where
    // jawt.dll cannot be found when JLinkNativeLibrary loads (the code
    // below will find it, though).
    if (hJawtLib == NULL || getAWTproc == NULL) {
        // Get OS version. On NT and later, treat path as Unicode.
        OSVERSIONINFO verInfo;
        verInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
        GetVersionEx(&verInfo);
        if (verInfo.dwMajorVersion > 4 || verInfo.dwPlatformId == VER_PLATFORM_WIN32_NT) {
            wchar_t buf[1024] = {0};  // Ensure all 0 chars for string operations below.
            const jchar* unicodePathStr = (*env)->GetStringChars(env, javaHomePath, NULL);
            int len = (*env)->GetStringLength(env, javaHomePath);
            wcsncpy(buf, unicodePathStr, len);
            wcscat(buf, L"\\bin\\jawt.dll");
            (*env)->ReleaseStringChars(env, javaHomePath, unicodePathStr);
            hJawtLib = LoadLibraryW(buf);
        } else {
            char buf[1024];
            const char* pathStr = (*env)->GetStringUTFChars(env, javaHomePath, NULL);
            strcpy(buf, pathStr);
            strcat(buf, "\\bin\\jawt.dll");
            (*env)->ReleaseStringUTFChars(env, javaHomePath, pathStr);
            hJawtLib = LoadLibrary(buf);
        }
        if (hJawtLib == NULL)
            return -11;
        getAWTproc = (GET_AWT_PROC) GetProcAddress(hJawtLib, "_JAWT_GetAWT@8");
        if (getAWTproc == NULL)
            return -12;
    }

    // Taken from http://java.sun.com/j2se/1.3/docs/guide/awt/AWT_Native_Interface.html
    awt.version = JAWT_VERSION_1_4;
    if ((*getAWTproc)(env, &awt) == JNI_FALSE)
        return -13;
    ds = awt.GetDrawingSurface(env, windowObj);
    if (ds == NULL)
        return -1;
    lock = ds->Lock(ds);
    if ((lock & JAWT_LOCK_ERROR) != 0) {
        awt.FreeDrawingSurface(ds);
        return -1;
    }
    dsi = ds->GetDrawingSurfaceInfo(ds);
    if (dsi == NULL) {
        ds->Unlock(ds);
        awt.FreeDrawingSurface(ds);
        return -1;
    }
    dsi_win = (JAWT_Win32DrawingSurfaceInfo*) dsi->platformInfo;
    if(dsi_win != NULL)
        hwnd = (jlong) dsi_win->hwnd;

    ds->FreeDrawingSurfaceInfo(dsi);
    ds->Unlock(ds);
    awt.FreeDrawingSurface(ds);

    return hwnd;
#else
    // Note that althuogh this is currently the correct implementation
    // for non-Windows platforms, this function is actually only called
    // under Windows.
    return -1;
#endif
}*/

/* Added as a convenience function for webMathematica. Not documented, not supported. */
/*killProcess
JNIEXPORT jint MLFUNC(killProcess), jlong pid) {

#ifdef WINDOWS_MATHLINK
    HANDLE phandle = OpenProcess(PROCESS_TERMINATE, FALSE, (int) pid);
    if (phandle == NULL)
        return -1;
    if (!TerminateProcess(phandle, 1))
        return GetLastError();
    CloseHandle(phandle);
    return 0;
#elif defined(UNIX_MATHLINK)
    if (kill((pid_t) pid, SIGKILL) == 0)
        return 0;
    else
        return errno;
#else
    // Not supported on classic MacOS.
    return -1;
#endif
}*/

/*appToFront
JNIEXPORT void MLFUNC(appToFront), jlong pid) {

#ifdef WINDOWS_MATHLINK
    BOOL CALLBACK appMainWindowToFrontCallback(HWND, LPARAM);
    WNDENUMPROC callbackFunc = (WNDENUMPROC) &appMainWindowToFrontCallback;

    // We call AttachThreadInput to get around restrictions in Win 98 and later on who is allowed to set the
    // foreground window. Java can't put its window to the front since it is not the foreground app, but this
    // trick lets us pretend that our thread is the foreground thread.
    DWORD foregroundThread = GetWindowThreadProcessId((HWND) pid, NULL);
    DWORD thisThread = GetCurrentThreadId();
    if (foregroundThread != thisThread)
        AttachThreadInput(foregroundThread, thisThread, TRUE);

    EnumWindows(callbackFunc, (LPARAM) pid);

    if (foregroundThread != thisThread)
        AttachThreadInput(foregroundThread, thisThread, FALSE);
#endif

#if 0  // DARWIN_MATHLINK, X86_DARWIN_MATHLINK
    // This is not a real implementation, just some code that is pasted
    // here temporarily to remind me of some of the Carbon functions for
    // working with processes. IIRC, the value given by $NotebookPRocessID on
    // Darwin doesn't look like a PSN as used in this code.
    ProcessSerialNumber psn;
    ProcessInfoRec infoRec;
    char name[256];
    infoRec.processInfoLength = sizeof(ProcessInfoRec);
    infoRec.processAppSpec = NULL;
    infoRec.processName = name;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        if (GetProcessInformation(&psn, &infoRec) == noErr) {
            if (infoRec.processType == 'APPL' && infoRec.processSignature == 'OMEG') {
                SetFrontProcess(&psn);
                return;
            }
        }
    }
#endif

}*/

/*appMainWindowToFrontCallback
#ifdef WINDOWS_MATHLINK

BOOL CALLBACK appMainWindowToFrontCallback(HWND hwnd, LPARAM lParam) {
    DWORD winProcID;
    GetWindowThreadProcessId(hwnd, &winProcID);
    // lParam is the procID of the app we want to bring to the foreground.
    if ((DWORD)lParam == winProcID && IsWindowVisible(hwnd))
        SetForegroundWindow(hwnd);
    return TRUE;
}

#endif*/

MLFUNCWITHARGS(getUseNumPy) {

    PyObject *ml;
    MLPARSEARGS("Op", &ml);
    MLINK mlink = _MLGetMLINK(ml);

    struct cookie* c = (struct cookie*) MLUserData(mlink, NULL);

    MLRETURNBOOL(c->useNumPy)

}

MLFUNCWITHARGS(setUseNumPy) {

    PyObject *ml;
    bool use_numpy;
    MLPARSEARGS("Op", &ml, &use_numpy);
    MLINK mlink = _MLGetMLINK(ml);
    // Py_XDECREF(ml);

    struct cookie* c = (struct cookie*) MLUserData(mlink, NULL);

    c->useNumPy = use_numpy;

    MLRETURNBOOL(use_numpy);
}

MLFUNCWITHARGS(setDebugLevel) {

    PyObject *ml;
    int level;
    MLPARSEARGS("Oi", &ml, &level);

    ML_DEBUG_LEVEL = level;

    Py_RETURN_NONE;

}
/******************************  PJLink Module Setup  ********************************/

static PyMethodDef PJLinkNativeLibraryMethods[] = {
    {"Initialize", PJLink_MLInitialize, METH_NOARGS, ""},
    {"OpenString", PJLink_MLOpenString, METH_VARARGS, ""},
    {"Open", PJLink_MLOpen, METH_VARARGS, ""},
    {"LoopbackOpen", PJLink_MLLoopbackOpen, METH_VARARGS, ""},
    {"SetEnvIDString", PJLink_MLSetEnvIDString, METH_VARARGS, ""},
    {"GetLinkedEnvIDString", PJLink_MLGetLinkedEnvIDString, METH_VARARGS, ""},
    {"Connect", PJLink_MLConnect, METH_VARARGS, ""},
    {"Close", PJLink_MLClose, METH_VARARGS, ""},
    {"Name", PJLink_MLName, METH_VARARGS, ""},
    {"SetYieldFunction", PJLink_MLSetYieldFunction, METH_VARARGS, ""},
    {"SetMessageHandler", PJLink_MLSetMessageHandler, METH_VARARGS, ""},
    {"NewPacket", PJLink_MLNewPacket, METH_VARARGS, ""},
    {"EndPacket", PJLink_MLEndPacket, METH_VARARGS, ""},
    {"NextPacket", PJLink_MLNextPacket, METH_VARARGS, ""},
    {"Error", PJLink_MLError, METH_VARARGS, ""},
    {"ErrorMessage", PJLink_MLErrorMessage, METH_VARARGS, ""},
    {"ClearError", PJLink_MLClearError, METH_VARARGS, ""},
    {"SetError", PJLink_MLSetError, METH_VARARGS, ""},
    {"Ready", PJLink_MLReady, METH_VARARGS, ""},
    {"Flush", PJLink_MLFlush, METH_VARARGS, ""},
    {"GetNext", PJLink_MLGetNext, METH_VARARGS, ""},
    {"GetType", PJLink_MLGetType, METH_VARARGS, ""},
    {"PutNext", PJLink_MLPutNext, METH_VARARGS, ""},
    {"GetArgCount", PJLink_MLGetArgCount, METH_VARARGS, ""},
    {"PutArgCount", PJLink_MLPutArgCount, METH_VARARGS, ""},
    {"GetString", PJLink_MLGetString, METH_VARARGS, ""},
    {"PutString", PJLink_MLPutString, METH_VARARGS, ""},
    {"GetByteString", PJLink_MLGetByteString, METH_VARARGS, ""},
    {"PutByteString", PJLink_MLPutByteString, METH_VARARGS, ""},
    {"GetSymbol", PJLink_MLGetSymbol, METH_VARARGS, ""},
    {"PutSymbol", PJLink_MLPutSymbol, METH_VARARGS, ""},
    {"GetInteger", PJLink_MLGetInteger, METH_VARARGS, ""},
    {"PutInteger", PJLink_MLPutInteger, METH_VARARGS, ""},
    {"GetDouble", PJLink_MLGetDouble, METH_VARARGS, ""},
    {"PutDouble", PJLink_MLPutDouble, METH_VARARGS, ""},
    {"CheckFunction", PJLink_MLCheckFunction, METH_VARARGS, ""},
    {"CheckFunctionWithArgCount", PJLink_MLCheckFunctionWithArgCount, METH_VARARGS, ""},
    {"GetData", PJLink_MLGetData, METH_VARARGS, ""},
    {"PutData", PJLink_MLPutData, METH_VARARGS, ""},
    {"BytesToGet", PJLink_MLBytesToGet, METH_VARARGS, ""},
    {"PutSize", PJLink_MLPutSize, METH_VARARGS, ""},
    {"BytesToPut", PJLink_MLBytesToPut, METH_VARARGS, ""},
    {"GetArray", PJLink_MLGetArray, METH_VARARGS, ""},
    {"PutArrayFlat", PJLink_MLPutArrayFlat, METH_VARARGS, ""},
    {"PutArray", PJLink_MLPutArray, METH_VARARGS, ""},
    //    {"_nativeSizesMatch", PJLink_nativeSizesMatch, METH_VARARGS, ""},
    {"TransferExpression", PJLink_MLTransferExpression, METH_VARARGS, ""},
    {"TransferToEndOfLoopbackLink", PJLink_MLTransferToEndOfLoopbackLink, METH_VARARGS, ""},
    {"GetMessage", PJLink_MLGetMessage, METH_VARARGS, ""},
    {"PutMessage", PJLink_MLPutMessage, METH_VARARGS, ""},
    {"MessageReady", PJLink_MLMessageReady, METH_VARARGS, ""},
    {"CreateMark", PJLink_MLCreateMark, METH_VARARGS, ""},
    {"SeekMark", PJLink_MLSeekMark, METH_VARARGS, ""},
    {"DestroyMark", PJLink_MLDestroyMark, METH_VARARGS, ""},
    //    {"_hideJavaWindow", PJLink_hideJavaWindow, METH_VARARGS, ""},
    //    {"_macJavaLayerToFront", PJLink_macJavaLayerToFront, METH_VARARGS, ""},
    //    {"_winJavaLayerToFront", PJLink_winJavaLayerToFront, METH_VARARGS, ""},
    //    {"_mathematicaToFront", PJLink_mathematicaToFront, METH_VARARGS, ""},
    //    {"_getNativeWindowHandle", PJLink_getNativeWindowHandle, METH_VARARGS, ""},
    //    {"_killProcess", PJLink_killProcess, METH_VARARGS, ""},
    //    {"_appToFront", PJLink_appToFront, METH_VARARGS, ""},
    {"getUseNumPy", PJLink_getUseNumPy, METH_VARARGS, ""},
    {"setUseNumPy", PJLink_setUseNumPy, METH_VARARGS, ""},
    {"setDebugLevel", PJLink_setDebugLevel, METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

const char PJLinkNativeLibrary_doc[] = "PJLinkNativeLibrary is a layer on MathLink for python porting JLink to the python C API";

static struct PyModuleDef PJLinkNativeLibraryModule = {
    PyModuleDef_HEAD_INIT,
    "PJLinkNativeLibrary",   /* name of module */
    PJLinkNativeLibrary_doc, /* module documentation, may be NULL */
    -1,       /* size of per-interpreter state of the module,
                 or -1 if the module keeps state in global variables. */
    PJLinkNativeLibraryMethods
};

PyMODINIT_FUNC PyInit_PJLinkNativeLibrary(void)
{
    return PyModule_Create(&PJLinkNativeLibraryModule);
}