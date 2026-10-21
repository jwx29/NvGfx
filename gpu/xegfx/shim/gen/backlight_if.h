/*
 * This file is @generated automatically.
 * Do not modify anything in here by hand.
 *
 * Created from source file
 *   sys/dev/backlight/backlight_if.m
 * with
 *   makeobjops.awk
 *
 * See the source file for legal information
 */


#ifndef _backlight_if_h_
#define _backlight_if_h_

/** @brief Unique descriptor for the BACKLIGHT_UPDATE_STATUS() method */
extern struct kobjop_desc backlight_update_status_desc;
/** @brief A function implementing the BACKLIGHT_UPDATE_STATUS() method */
typedef int backlight_update_status_t(device_t dev,
                                      struct backlight_props *props);

static __inline int
BACKLIGHT_UPDATE_STATUS(device_t dev,
                                            struct backlight_props *props)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)dev)->ops, backlight_update_status);
	rc = ((backlight_update_status_t *) _m)(dev, props);
	return (rc);
}

/** @brief Unique descriptor for the BACKLIGHT_GET_STATUS() method */
extern struct kobjop_desc backlight_get_status_desc;
/** @brief A function implementing the BACKLIGHT_GET_STATUS() method */
typedef int backlight_get_status_t(device_t dev, struct backlight_props *props);

static __inline int
BACKLIGHT_GET_STATUS(device_t dev,
                                         struct backlight_props *props)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)dev)->ops, backlight_get_status);
	rc = ((backlight_get_status_t *) _m)(dev, props);
	return (rc);
}

/** @brief Unique descriptor for the BACKLIGHT_GET_INFO() method */
extern struct kobjop_desc backlight_get_info_desc;
/** @brief A function implementing the BACKLIGHT_GET_INFO() method */
typedef int backlight_get_info_t(device_t dev, struct backlight_info *info);

static __inline int
BACKLIGHT_GET_INFO(device_t dev,
                                       struct backlight_info *info)
{
	kobjop_t _m;
	int rc;
	KOBJOPLOOKUP(((kobj_t)dev)->ops, backlight_get_info);
	rc = ((backlight_get_info_t *) _m)(dev, info);
	return (rc);
}

#endif /* _backlight_if_h_ */
