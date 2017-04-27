#pragma once

#include <stdbool.h>
#include <string.h>

struct hash;

struct hash_iter {
	const struct hash *hash;
	unsigned int bucket;
	unsigned int entry;
};

struct hash *hash_new(unsigned int n_buckets, void (*free_value)(void *value));
void hash_free(struct hash *hash);
int hash_add_bin(struct hash *hash,
		 const char *key, size_t keylen, const void *value);
int hash_add_unique_bin(struct hash *hash,
			const char *key, size_t keylen, const void *value);
int hash_del_bin(struct hash *hash, const char *key, size_t keylen);
void *hash_find_bin(const struct hash *hash, const char *key, size_t keylen);
unsigned int hash_get_count(const struct hash *hash);
void hash_iter_init(const struct hash *hash, struct hash_iter *iter);
bool hash_iter_next_bin(struct hash_iter *iter, const char **key, size_t *keylen,
							const void **value);


static inline int hash_add(struct hash *hash, const char *key, const void *value)
{
	size_t keylen = strlen(key);
	return hash_add_bin(hash, key, keylen, value);
}
static inline int hash_add_unique(struct hash *hash,
				  const char *key, const void *value)
{
	size_t keylen = strlen(key);
	return hash_add_unique_bin(hash, key, keylen, value);
}
static inline int hash_del(struct hash *hash, const char *key)
{
	size_t keylen = strlen(key);
	return hash_del_bin(hash, key, keylen);
}
static inline void *hash_find(const struct hash *hash, const char *key)
{
	size_t keylen = strlen(key);
	return hash_find_bin(hash, key, keylen);
}
static inline bool hash_iter_next(struct hash_iter *iter, const char **key,
				  const void **value)
{
	return hash_iter_next_bin(iter, key, NULL, value);
}
