/* radare - LGPL - Copyright 2019 - thestr4ng3r */

#include "R2Scope.h"
#include "R2Architecture.h"
#include "R2Utils.h"

#include <funcdata.hh>

#include <r_anal.h>

R2Scope::R2Scope(R2Architecture *arch)
		: Scope("", arch),
		  arch(arch),
		  cache(new ScopeInternal("radare2-internal", arch))
{
}

R2Scope::~R2Scope()
{
	delete cache;
}

FunctionSymbol *R2Scope::registerFunction(RAnalFunction *fcn) const
{
	RCore *core = arch->getCore();

	auto sym = cache->addFunction(Address(arch->getDefaultSpace(), fcn->addr), fcn->name);
	auto function = sym->getFunction();

	RList *vars = r_anal_var_list (core->anal, fcn, 'b');
	auto stackSpace = arch->getStackSpace();
	if(vars)
	{
		r_list_foreach_cpp<RAnalVar>(vars, [this, function, stackSpace](RAnalVar *var) {
			Datatype *type = arch->types->findByName(var->type);
			if(!type)
			{
				eprintf("type not found %s\n", var->type);
				type = arch->types->findByName("uint32_t");
			}
			uintb off;
			if(var->delta >= 0)
				off = var->delta;
			else
				off = stackSpace->getHighest() + var->delta + 1;
			auto addr = Address(stackSpace, off);
			auto overlap = function->getScopeLocal()->findOverlap(addr, type->getSize());
			if(overlap)
			{
				eprintf("overlap for %s\n", var->name);
				return;
			}
			auto varsym = function->getScopeLocal()->addSymbol(var->name, type, addr, Address());
			function->getScopeLocal()->setAttribute(varsym->getSymbol(), Varnode::namelock | Varnode::typelock);
		});
		r_list_free(vars);
	}

	return sym;
}

Symbol *R2Scope::registerFlag(RFlagItem *flag) const
{
	uint4 attr = Varnode::namelock | Varnode::typelock;
	Datatype *type = nullptr;
	if(flag->space && !strcmp(flag->space->name, R_FLAGS_FS_STRINGS))
	{
		Datatype *ptype = arch->types->findByName("char");
		type = arch->types->getTypeArray(static_cast<int4>(flag->size), ptype);
		attr |= Varnode::readonly;
	}

	// TODO: more types

	if(!type)
	{
		type = arch->types->getTypeCode();
	}

	SymbolEntry *entry = cache->addSymbol(flag->name, type, Address(arch->getDefaultSpace(), flag->offset), Address());
	if(!entry)
		return nullptr;

	auto symbol = entry->getSymbol();
	cache->setAttribute(symbol, attr);

	return symbol;
}

Symbol *R2Scope::queryR2Absoulte(ut64 addr) const
{
	// TODO: sync
	RCore *core = arch->getCore();
	RAnalFunction *fcn = r_anal_get_fcn_at(core->anal, addr, R_ANAL_FCN_TYPE_NULL);
	if(fcn)
		return registerFunction(fcn);

	// TODO: register more things

	RFlagItem *flag = r_flag_get_at(core->flags, addr, false);
	if(flag)
		return registerFlag(flag);

	return nullptr;
}


Symbol *R2Scope::queryR2(const Address &addr) const
{
	switch(addr.getSpace()->getType())
	{
		case IPTR_CONSTANT:
			break;
		case IPTR_PROCESSOR:
			return queryR2Absoulte(addr.getOffset());
		case IPTR_SPACEBASE:
			break;
		case IPTR_INTERNAL:
			break;
		case IPTR_FSPEC:
			break;
		case IPTR_IOP:
			break;
		case IPTR_JOIN:
			break;
	}
	return nullptr;
}

LabSymbol *R2Scope::queryR2FunctionLabel(const Address &addr) const
{
	// TODO: sync
	RCore *core = arch->getCore();

	RAnalFunction *fcn = r_anal_get_fcn_in(core->anal, addr.getOffset(), R_ANAL_FCN_TYPE_NULL);
	if(!fcn)
		return nullptr;

	const char *label = r_anal_fcn_label_at(core->anal, fcn, addr.getOffset());
	if(!label)
		return nullptr;

	return cache->addCodeLabel(addr, label);
}

SymbolEntry *R2Scope::findAddr(const Address &addr, const Address &usepoint) const
{
	SymbolEntry *entry = cache->findAddr(addr,usepoint);
	if(entry)
		return entry->getAddr() == addr ? entry : nullptr;

	entry = cache->findContainer(addr, 1, Address());
	if(entry) // Address is already queried, but symbol doesn't start at our address
		return nullptr;

	Symbol *sym = queryR2(addr);
	entry = sym ? sym->getMapEntry(addr) : nullptr;

	return (entry && entry->getAddr() == addr) ? entry : nullptr;
}

SymbolEntry *R2Scope::findContainer(const Address &addr, int4 size, const Address &usepoint) const
{
	SymbolEntry *entry = cache->findClosestFit(addr, size, usepoint);

	if(!entry)
	{
		Symbol *sym = queryR2(addr);
		entry = sym ? sym->getMapEntry(addr) : nullptr;
	}

	if(entry)
	{
		// Entry contains addr, does it contain addr+size
		uintb last = entry->getAddr().getOffset() + entry->getSize() - 1;
		if (last < addr.getOffset() + size - 1)
			return nullptr;
	}

	return entry;
}

Funcdata *R2Scope::findFunction(const Address &addr) const
{
	Funcdata *fd = cache->findFunction(addr);
	if(fd)
		return fd;

	// Check if this address has already been queried,
	// (returning a symbol other than a function_symbol)
	if(cache->findContainer(addr, 1, Address()))
		return nullptr;

	FunctionSymbol *sym;
	sym = dynamic_cast<FunctionSymbol *>(queryR2(addr));
	if(sym)
		return sym->getFunction();

	return nullptr;
}

ExternRefSymbol *R2Scope::findExternalRef(const Address &addr) const
{
	ExternRefSymbol *sym = cache->findExternalRef(addr);
	if(sym)
		return sym;

	// Check if this address has already been queried,
	// (returning a symbol other than an external ref symbol)
	if(cache->findContainer(addr, 1, Address()))
		return nullptr;

	return dynamic_cast<ExternRefSymbol *>(queryR2(addr));
}

LabSymbol *R2Scope::findCodeLabel(const Address &addr) const
{
	LabSymbol *sym = cache->findCodeLabel(addr);
	if(sym)
		return sym;

	// Check if this address has already been queried,
	// (returning a symbol other than a code label)
	SymbolEntry *entry = cache->findAddr(addr,Address());
	if(entry)
		return nullptr;

	return queryR2FunctionLabel(addr);
}

Funcdata *R2Scope::resolveExternalRefFunction(ExternRefSymbol *sym) const
{
	return queryFunction(sym->getRefAddr());
}