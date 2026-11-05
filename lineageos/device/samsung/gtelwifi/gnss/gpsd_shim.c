/*
 * Shim for the Broadcom glgps daemon (gpsd) and gps.default.so of the
 * SM-T560 stock firmware (Android 4.4). Both are kept unchanged except
 * for their DT_NEEDED lists (see README): this library replaces libgui,
 * libstlport and libicuuc, of which the blobs use only the symbols below.
 *
 * - STLport's node allocator: plain malloc/free.
 * - android::SensorManager (libgui, KitKat ABI): glgps can feed device
 *   sensors into its engine; here the sensor list is empty, so it runs
 *   without them.
 * - Two OpenSSL calls BoringSSL lacks, used only by the LTO download over
 *   TLS (disabled in gps.xml, the server is gone).
 *
 * The C++ names are the mangled symbols; the calling convention follows
 * the ARM Itanium C++ ABI (this in r0, or the result pointer in r0 and
 * this in r1 for a by-value class result).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

/* std::__node_alloc::_M_allocate(size_t&), _M_deallocate(void*, size_t) */
void *_ZNSt12__node_alloc11_M_allocateERj(size_t *n)
{
	return malloc(*n);
}

void _ZNSt12__node_alloc13_M_deallocateEPvj(void *p, size_t n)
{
	(void)n;
	free(p);
}

/* android::Singleton<SensorManager>::sLock (Mutex) and ::sInstance */
uint32_t _ZN7android9SingletonINS_13SensorManagerEE5sLockE[4];
void *_ZN7android9SingletonINS_13SensorManagerEE9sInstanceE;

/* SensorManager::SensorManager() */
void *_ZN7android13SensorManagerC1Ev(void *thiz)
{
	return thiz;
}

/* ssize_t SensorManager::getSensorList(Sensor const* const** list) const */
ssize_t _ZNK7android13SensorManager13getSensorListEPPKPKNS_6SensorE(const void *thiz,
								    const void *const **list)
{
	static const void *const empty[1];

	(void)thiz;
	if (list)
		*list = empty;
	return 0;
}

/* Sensor const* SensorManager::getDefaultSensor(int type) */
const void *_ZN7android13SensorManager16getDefaultSensorEi(void *thiz, int type)
{
	(void)thiz;
	(void)type;
	return NULL;
}

/* sp<SensorEventQueue> SensorManager::createEventQueue() */
void _ZN7android13SensorManager16createEventQueueEv(void **result, void *thiz)
{
	(void)thiz;
	*result = NULL;
}

/* SensorEventQueue: never created, kept for the dynamic linker */
ssize_t _ZN7android16SensorEventQueue4readEP12ASensorEventj(void *thiz, void *events,
							    size_t num)
{
	(void)thiz;
	(void)events;
	(void)num;
	return -1;
}

int _ZNK7android16SensorEventQueue12enableSensorEPKNS_6SensorE(const void *thiz,
								const void *sensor)
{
	(void)thiz;
	(void)sensor;
	return -1;
}

int _ZNK7android16SensorEventQueue13disableSensorEPKNS_6SensorE(const void *thiz,
								 const void *sensor)
{
	(void)thiz;
	(void)sensor;
	return -1;
}

int _ZNK7android16SensorEventQueue12setEventRateEPKNS_6SensorEx(const void *thiz,
								const void *sensor, int64_t ns)
{
	(void)thiz;
	(void)sensor;
	(void)ns;
	return -1;
}

int _ZNK7android16SensorEventQueue5getFdEv(const void *thiz)
{
	(void)thiz;
	return -1;
}

/* Sensor getters: no Sensor object is ever handed out */
static const char empty_name[] = "";
static const char *const empty_string8 = empty_name;

const void *_ZNK7android6Sensor7getNameEv(const void *thiz)
{
	(void)thiz;
	return &empty_string8;
}

const void *_ZNK7android6Sensor9getVendorEv(const void *thiz)
{
	(void)thiz;
	return &empty_string8;
}

int _ZNK7android6Sensor9getHandleEv(const void *thiz)
{
	(void)thiz;
	return 0;
}

int _ZNK7android6Sensor7getTypeEv(const void *thiz)
{
	(void)thiz;
	return 0;
}

float _ZNK7android6Sensor11getMaxValueEv(const void *thiz)
{
	(void)thiz;
	return 0.0f;
}

float _ZNK7android6Sensor13getResolutionEv(const void *thiz)
{
	(void)thiz;
	return 0.0f;
}

float _ZNK7android6Sensor13getPowerUsageEv(const void *thiz)
{
	(void)thiz;
	return 0.0f;
}

int32_t _ZNK7android6Sensor11getMinDelayEv(const void *thiz)
{
	(void)thiz;
	return 0;
}

/* OpenSSL 1.0 calls missing from BoringSSL */
const void *SSLv3_client_method(void)
{
	return NULL;
}

long SSL_CTX_ctrl(void *ctx, int cmd, long larg, void *parg)
{
	(void)ctx;
	(void)cmd;
	(void)larg;
	(void)parg;
	return 0;
}
