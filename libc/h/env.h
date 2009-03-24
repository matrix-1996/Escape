/**
 * @version		$Id$
 * @author		Nils Asmussen <nils@script-solution.de>
 * @copyright	2008 Nils Asmussen
 */

#ifndef ENV_H_
#define ENV_H_

#include "common.h"

/**
 * Returns the env-variable-name with given index
 *
 * @param index the index
 * @return the name of it or NULL if the index does not exist (or it failed for another reason)
 */
char *getEnvByIndex(u32 index);

/**
 * Returns the value of the given environment-variable. Note that you have to copy the value
 * if you want to keep it!
 *
 * @param name the environment-variable-name
 * @return the value
 */
char *getEnv(const char *name);

/**
 * Sets the environment-variable <name> to <value>.
 *
 * @param name the name
 * @param value the value
 */
void setEnv(const char *name,const char *value);

#endif /* ENV_H_ */
