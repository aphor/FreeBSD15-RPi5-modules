/*
 * This file is @generated automatically.
 * Do not modify anything in here by hand.
 *
 * Created from source file
 *   /usr/src/sys/dev/pwm/pwmbus_if.m
 * with
 *   makeobjops.awk
 *
 * See the source file for legal information
 */


#ifndef _pwmbus_if_h_
#define _pwmbus_if_h_

/** @brief Unique descriptor for the PWMBUS_CHANNEL_CONFIG() method */
extern struct kobjop_desc pwmbus_channel_config_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_CONFIG() method */
typedef int pwmbus_channel_config_t(device_t bus, u_int channel, u_int period,
                                    u_int duty);

static __inline int PWMBUS_CHANNEL_CONFIG(device_t bus, u_int channel,
                                          u_int period, u_int duty)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_config);
	rc = ((pwmbus_channel_config_t *) _m)(bus, channel, period, duty);
	return (rc);
}

/** @brief Unique descriptor for the PWMBUS_CHANNEL_GET_CONFIG() method */
extern struct kobjop_desc pwmbus_channel_get_config_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_GET_CONFIG() method */
typedef int pwmbus_channel_get_config_t(device_t bus, u_int channel,
                                        u_int *period, u_int *duty);

static __inline int PWMBUS_CHANNEL_GET_CONFIG(device_t bus, u_int channel,
                                              u_int *period, u_int *duty)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_get_config);
	rc = ((pwmbus_channel_get_config_t *) _m)(bus, channel, period, duty);
	return (rc);
}

/** @brief Unique descriptor for the PWMBUS_CHANNEL_SET_FLAGS() method */
extern struct kobjop_desc pwmbus_channel_set_flags_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_SET_FLAGS() method */
typedef int pwmbus_channel_set_flags_t(device_t bus, u_int channel,
                                       uint32_t flags);

static __inline int PWMBUS_CHANNEL_SET_FLAGS(device_t bus, u_int channel,
                                             uint32_t flags)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_set_flags);
	rc = ((pwmbus_channel_set_flags_t *) _m)(bus, channel, flags);
	return (rc);
}

/** @brief Unique descriptor for the PWMBUS_CHANNEL_GET_FLAGS() method */
extern struct kobjop_desc pwmbus_channel_get_flags_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_GET_FLAGS() method */
typedef int pwmbus_channel_get_flags_t(device_t bus, u_int channel,
                                       uint32_t *flags);

static __inline int PWMBUS_CHANNEL_GET_FLAGS(device_t bus, u_int channel,
                                             uint32_t *flags)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_get_flags);
	rc = ((pwmbus_channel_get_flags_t *) _m)(bus, channel, flags);
	return (rc);
}

/** @brief Unique descriptor for the PWMBUS_CHANNEL_ENABLE() method */
extern struct kobjop_desc pwmbus_channel_enable_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_ENABLE() method */
typedef int pwmbus_channel_enable_t(device_t bus, u_int channel, bool enable);

static __inline int PWMBUS_CHANNEL_ENABLE(device_t bus, u_int channel,
                                          bool enable)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_enable);
	rc = ((pwmbus_channel_enable_t *) _m)(bus, channel, enable);
	return (rc);
}

/** @brief Unique descriptor for the PWMBUS_CHANNEL_IS_ENABLED() method */
extern struct kobjop_desc pwmbus_channel_is_enabled_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_IS_ENABLED() method */
typedef int pwmbus_channel_is_enabled_t(device_t bus, u_int channel,
                                        bool *enabled);

static __inline int PWMBUS_CHANNEL_IS_ENABLED(device_t bus, u_int channel,
                                              bool *enabled)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_is_enabled);
	rc = ((pwmbus_channel_is_enabled_t *) _m)(bus, channel, enabled);
	return (rc);
}

/** @brief Unique descriptor for the PWMBUS_CHANNEL_COUNT() method */
extern struct kobjop_desc pwmbus_channel_count_desc;
/** @brief A function implementing the PWMBUS_CHANNEL_COUNT() method */
typedef int pwmbus_channel_count_t(device_t bus, u_int *nchannel);

static __inline int PWMBUS_CHANNEL_COUNT(device_t bus, u_int *nchannel)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)bus)->ops,pwmbus_channel_count);
	rc = ((pwmbus_channel_count_t *) _m)(bus, nchannel);
	return (rc);
}

#endif /* _pwmbus_if_h_ */
