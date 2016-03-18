#ifndef KSYMTAB_H_
#define	KABI_ELF_H_

extern void free_ksymtab(char **, size_t);
extern char **read_ksymtab(char *, size_t *);

#endif /* KSYMTAB_H_ */
