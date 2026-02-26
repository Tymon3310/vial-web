#define PY_SSIZE_T_CLEAN

#include "Python.h"

#include <emscripten.h>
#include <emscripten/threading.h>

static PyMethodDef PyQt5Methods[] = {
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef PyQt5 = {
    PyModuleDef_HEAD_INIT,
    "PyQt5",
    0,
    -1,
    PyQt5Methods
};

PyMODINIT_FUNC PyInit_PyQt5(void) {
    PyObject *mod = PyModule_Create(&PyQt5);
    PyModule_AddObject(mod, "__path__", Py_BuildValue("()"));
    return mod;
}

extern PyObject *PyInit_sip();
extern PyObject *PyInit_QtCore();
extern PyObject *PyInit_QtGui();
extern PyObject *PyInit_QtWidgets();
void execLastQApp();

static volatile int glue_error, glue_ready;
static uint8_t glue_response[32];

static PyObject * vialglue_write_device(PyObject *self, PyObject *args) {
    const uint8_t *data;
    Py_ssize_t size;

    if (!PyArg_ParseTuple(args, "y#", &data, &size))
        return NULL;

    if (size != 33)
        return NULL;

    // printf("request to send data of size %d starting with %02X %02X %02X %02X\n", size, data[0], data[1], data[2], data[3]);

    glue_ready = 0;

    EM_ASM({
        vialgluejs_write_device($0);
    }, data + 1);


    return PyLong_FromLong(33); // TODO: wait until successfully sent?
}

static PyObject * vialglue_read_device(PyObject *self, PyObject *args) {
    while (!glue_ready) {
        // usleep(5); // spinlock is way more responsive, esp. in matrix tester
    }

    size_t len = sizeof(glue_response);

    if (glue_error)
        len = 0;

    return PyBytes_FromStringAndSize(glue_response, len);
}

void vialglue_set_response(uint8_t *data) {
    memcpy(glue_response, data, sizeof(glue_response));

    glue_error = 0;
    glue_ready = 1;
}

void vialglue_set_response_error(uint8_t *data) {
    glue_error = 1;
    glue_ready = 1;
}

const char *g_device_desc;

void vialglue_set_device_desc(const char *s) {
    g_device_desc = s;
}

static PyObject * vialglue_unlock_start(PyObject *self, PyObject *args) {
    const uint8_t *data;
    Py_ssize_t size;
    unsigned long width, height;

    if (!PyArg_ParseTuple(args, "y#kk", &data, &size, &width, &height))
        return NULL;

    EM_ASM({
        vialgluejs_unlock_start($0, $1, $2, $3);
    }, data, size, width, height);

    return PyLong_FromLong(0);
}

static PyObject * vialglue_unlock_status(PyObject *self, PyObject *args) {
    unsigned long status;

    if (!PyArg_ParseTuple(args, "k", &status))
        return NULL;

    EM_ASM({
        postMessage({cmd: "unlock_status", data: $0});
    }, status);

    return PyLong_FromLong(0);
}

static PyObject * vialglue_unlock_done(PyObject *self, PyObject *args) {
    EM_ASM({
        postMessage({cmd: "unlock_done"});
    });

    return PyLong_FromLong(0);
}

static PyObject * vialglue_notify_ready(PyObject *self, PyObject *args) {
    EM_ASM({
        postMessage({cmd: "notify_ready"});
    });

    return PyLong_FromLong(0);
}

static PyObject* vialglue_get_device_desc(PyObject *self, PyObject *args) {
    return PyUnicode_FromString(g_device_desc);
}

static PyObject * vialglue_load_layout(PyObject *self, PyObject *args) {
    EM_ASM({
        postMessage({cmd: "load_layout"});
    });

    return PyLong_FromLong(0);
}

static PyObject * vialglue_load_firmware_bin(PyObject *self, PyObject *args) {
    EM_ASM({
        postMessage({cmd: "load_firmware_bin"});
    });

    return PyLong_FromLong(0);
}

static PyObject * vialglue_save_layout(PyObject *self, PyObject *args) {
    const uint8_t *data;

    if (!PyArg_ParseTuple(args, "y", &data))
        return NULL;

    EM_ASM({
        postMessage({cmd: "save_layout", layout: UTF8ToString($0)});
    }, data);

    return PyLong_FromLong(0);
}

// ── DFU flash bridge ────────────────────────────────────────────────────────
// Python calls dfu_flash_start(firmware_bytes) to kick off a WebUSB DFU flash.
// It then calls dfu_flash_status() in a loop to get progress/done/error.
// The main thread (index.html) posts back {cmd:"dfu_status", json:"..."} to the
// worker, which calls vialglue_set_dfu_status() below.

static volatile int       dfu_status_ready  = 0;
static volatile int       dfu_status_error  = 0;
// JSON payload: {"status":"progress","pct":0.5} | {"status":"done"} | {"status":"error","msg":"..."}
static char               dfu_status_buf[512];

// Called from JS to get the WASM address of dfu_status_ready so JS can
// Atomics.notify() after posting the status, waking emscripten_futex_wait().
EMSCRIPTEN_KEEPALIVE
int vialglue_dfu_status_ready_addr(void) {
    return (int)(uintptr_t)&dfu_status_ready;
}

void vialglue_set_dfu_status(const char *json) {
    strncpy(dfu_status_buf, json, sizeof(dfu_status_buf) - 1);
    dfu_status_buf[sizeof(dfu_status_buf) - 1] = '\0';
    dfu_status_error = 0;
    // Use __atomic_store_n so the Atomics.notify() on the JS side sees the
    // updated value atomically (WASM memory is a SharedArrayBuffer).
    __atomic_store_n(&dfu_status_ready, 1, __ATOMIC_SEQ_CST);
    // Wake any thread waiting in emscripten_futex_wait() on this address.
    emscripten_futex_wake(&dfu_status_ready, 1);
}

// Called from Python: dfu_flash_start(firmware_bytes) -> None
// Sends the firmware buffer to the main thread to begin WebUSB DFU.
static PyObject * vialglue_dfu_flash_start(PyObject *self, PyObject *args) {
    const uint8_t *data;
    Py_ssize_t size;

    if (!PyArg_ParseTuple(args, "y#", &data, &size))
        return NULL;

    __atomic_store_n(&dfu_status_ready, 0, __ATOMIC_SEQ_CST);
    dfu_status_error = 0;

    EM_ASM({
        vialgluejs_dfu_flash_start($0, $1);
    }, data, (int)size);

    return PyLong_FromLong(0);
}

// Called from Python (polling loop): dfu_flash_status() -> str (JSON)
// Waits until the main thread delivers a status update.
// emscripten_futex_wait() uses Atomics.wait() under the hood — it suspends
// this pthread without blocking the worker's JS event loop, so the incoming
// dfu_status postMessage (handled by worker.js onmessage) can be processed
// and call _vialglue_set_dfu_status / emscripten_futex_wake().
static PyObject * vialglue_dfu_flash_status(PyObject *self, PyObject *args) {
    while (!__atomic_load_n(&dfu_status_ready, __ATOMIC_SEQ_CST)) {
        emscripten_futex_wait(&dfu_status_ready, 0, 1e9);
    }
    __atomic_store_n(&dfu_status_ready, 0, __ATOMIC_SEQ_CST);
    return PyUnicode_FromString(dfu_status_buf);
}

// Called from Python: dfu_request_usb() -> None
// Triggers navigator.usb.requestDevice() on the main thread (needs user gesture
// to have been set up; the main thread will pop a "Connect DFU device" dialog).
static PyObject * vialglue_dfu_request_usb(PyObject *self, PyObject *args) {
    __atomic_store_n(&dfu_status_ready, 0, __ATOMIC_SEQ_CST);
    dfu_status_error = 0;

    EM_ASM({
        postMessage({cmd: "dfu_request_usb"});
    });

    return PyLong_FromLong(0);
}

// Called from Python: dfu_show_usb_picker() -> None
// Shows a native browser button overlay so the user can trigger
// navigator.usb.requestDevice() via a real user gesture (button click).
static PyObject * vialglue_dfu_show_usb_picker(PyObject *self, PyObject *args) {
    EM_ASM({
        postMessage({cmd: "dfu_show_usb_picker"});
    });

    return PyLong_FromLong(0);
}

// Called from Python: dfu_show_hid_picker() -> None
// Shows a native browser button overlay so the user can trigger
// navigator.hid.requestDevice() via a real user gesture (button click).
// Fire-and-forget: returns immediately; call request_reconnect() afterwards
// to block until the user clicks the button and the result is posted back.
static PyObject * vialglue_dfu_show_hid_picker(PyObject *self, PyObject *args) {
    __atomic_store_n(&dfu_status_ready, 0, __ATOMIC_SEQ_CST);
    dfu_status_error = 0;

    EM_ASM({
        postMessage({cmd: "dfu_show_hid_picker"});
    });

    return PyLong_FromLong(0);
}

// Called from Python after dfu_show_hid_picker() to wait for the WebHID
// reconnect result.  Uses emscripten_futex_wait() (Atomics.wait) so the
// worker event loop can receive the dfu_status postMessage from the main
// thread and call _vialglue_set_dfu_status / emscripten_futex_wake().
static PyObject * vialglue_request_reconnect(PyObject *self, PyObject *args) {
    while (!__atomic_load_n(&dfu_status_ready, __ATOMIC_SEQ_CST)) {
        emscripten_futex_wait(&dfu_status_ready, 0, 1e9);
    }
    __atomic_store_n(&dfu_status_ready, 0, __ATOMIC_SEQ_CST);
    return PyUnicode_FromString(dfu_status_buf);
}

static PyObject* vialglue_fatal_error(PyObject *self, PyObject *args) {
    const char *msg;

    if (!PyArg_ParseTuple(args, "s", &msg))
        return NULL;

    EM_ASM({
        postMessage({cmd: "fatal_error", msg: UTF8ToString($0)});
    }, msg);

    return PyLong_FromLong(0);
}

static PyMethodDef VialglueMethods[] = {
    {"write_device",  vialglue_write_device, METH_VARARGS, ""},
    {"read_device",  vialglue_read_device, METH_VARARGS, ""},
    {"unlock_start",  vialglue_unlock_start, METH_VARARGS, ""},
    {"unlock_status",  vialglue_unlock_status, METH_VARARGS, ""},
    {"unlock_done",  vialglue_unlock_done, METH_VARARGS, ""},
    {"notify_ready",  vialglue_notify_ready, METH_VARARGS, ""},
    {"get_device_desc",  vialglue_get_device_desc, METH_VARARGS, ""},
    {"load_layout",  vialglue_load_layout, METH_VARARGS, ""},
    {"load_firmware_bin", vialglue_load_firmware_bin, METH_VARARGS, ""},
    {"save_layout",  vialglue_save_layout, METH_VARARGS, ""},
    {"fatal_error",  vialglue_fatal_error, METH_VARARGS, ""},
    {"dfu_flash_start",    vialglue_dfu_flash_start,    METH_VARARGS, ""},
    {"dfu_flash_status",   vialglue_dfu_flash_status,   METH_VARARGS, ""},
    {"dfu_request_usb",    vialglue_dfu_request_usb,    METH_VARARGS, ""},
    {"dfu_show_usb_picker", vialglue_dfu_show_usb_picker, METH_VARARGS, ""},
    {"dfu_show_hid_picker", vialglue_dfu_show_hid_picker, METH_VARARGS, ""},
    {"request_reconnect",  vialglue_request_reconnect,  METH_VARARGS, ""},
    {NULL, NULL, 0, NULL}
};


static struct PyModuleDef vialgluemodule = {
    PyModuleDef_HEAD_INIT,
    "vialglue",
    NULL,
    -1,
    VialglueMethods
};

PyMODINIT_FUNC PyInit_vialglue(void) {
    return PyModule_Create(&vialgluemodule);
}

int main(int argc, char **argv) {
    PyImport_AppendInittab("vialglue", PyInit_vialglue);
    PyImport_AppendInittab("PyQt5", PyInit_PyQt5);
    PyImport_AppendInittab("PyQt5.sip", PyInit_sip);
    PyImport_AppendInittab("PyQt5.Qt", PyInit_QtCore);
    PyImport_AppendInittab("PyQt5.QtCore", PyInit_QtCore);
    PyImport_AppendInittab("PyQt5.QtGui", PyInit_QtGui);
    PyImport_AppendInittab("PyQt5.QtWidgets", PyInit_QtWidgets);

    Py_Initialize();

    // Fix import system to accommodate the shallow PyQt5 mock module
    // Thanks to dgym @ https://stackoverflow.com/questions/39250524/programmatically-define-a-package-structure-in-embedded-python-3
    PyRun_SimpleString(
        "from importlib import abc, machinery \n" \
        "import sys\n" \
        "\n" \
        "class Finder(abc.MetaPathFinder):\n" \
        "    def find_spec(self, fullname, path, target=None):\n" \
        "        if fullname in sys.builtin_module_names:\n" \
        "            return machinery.ModuleSpec(fullname, machinery.BuiltinImporter)\n" \
        "\n" \
        "sys.meta_path.append(Finder())\n" \
    );

    PyRun_SimpleString("from PyQt5.QtWidgets import QApplication\nqtApp = QApplication([\"pyodide\"])\n");

    EM_ASM({
        postMessage({cmd: "notify_alive"});
    });
    execLastQApp();

    return 0;
}
