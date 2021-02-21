#include "Dumper.h"
#include "Path.h"

bool VTHelper::IsValid(void* VTable_start, SectionInfo* sectionInfo)
{
	uintptr_t* vftable_ptr = reinterpret_cast<uintptr_t*>(VTable_start);
	uintptr_t* meta_ptr = vftable_ptr - 1;
	if (sectionInfo->RDATA.base <= *meta_ptr && *meta_ptr <= sectionInfo->RDATA.end) {
		if (sectionInfo->TEXT.base <= *vftable_ptr && *vftable_ptr <= sectionInfo->TEXT.end) {
			CompleteObjectLocator* COL = reinterpret_cast<CompleteObjectLocator*>(*meta_ptr);
#ifdef _WIN64
			if (COL->signature == 1) {
				auto TypeDesc = COL->GetTypeDescriptor();
#else
			if (COL->signature == 0) {
				auto TypeDesc = COL->pTypeDescriptor;
#endif
				if (IsBadReadPointer(reinterpret_cast<void*>(TypeDesc)) != 0) {
					return false;
				}
				// Test if string is ".?AV" real quick
				// I do this because using strcmp is probably slower.
				if (*reinterpret_cast<unsigned long*>(&TypeDesc->name) == TYPEDESCRIPTOR_SIGNITURE) {
					return true;
				}
			}
		}
	}
	return false;
}

vector<uintptr_t> VTHelper::GetListOfFunctions(void* VTable_start, SectionInfo* sectionInfo)
{
	vector<uintptr_t> functionList;
	uintptr_t* vftable_ptr = reinterpret_cast<uintptr_t*>(VTable_start);
	uintptr_t function_ptr = *vftable_ptr;
	while (sectionInfo->TEXT.base <= function_ptr && function_ptr <= sectionInfo->TEXT.end) {
		functionList.push_back(function_ptr);
		vftable_ptr++;
		function_ptr = *vftable_ptr;
	}
	return functionList;
}

vector<uintptr_t> VTHelper::FindAll(SectionInfo* sectionInfo)
{
	vector<uintptr_t> VTableList;
	uintptr_t ptr = sectionInfo->RDATA.base + sizeof(uintptr_t);
	while (ptr < sectionInfo->RDATA.end) {
		if (IsValid(reinterpret_cast<void*>(ptr), sectionInfo)) {
			VTableList.push_back(ptr);
		}
		ptr += sizeof(uintptr_t);
	}
	return VTableList;
}

const string VFTableSymbolStart = "??_7";
const string VFTableSymbolEnd = "6B@";
static char buff[0x1000];

string DemangleMSVC(char* symbol)
{
	memset(buff, 0, 0x1000);
	char* pSymbol = symbol;
	if (*(char*)(symbol + 4) == '?') pSymbol = symbol + 1;
	else if (*(char*)symbol == '.') pSymbol = symbol + 4;
	else if (*(char*)symbol == '?') pSymbol = symbol + 2;
	else
	{
		g_console.WriteBold("invalid msvc mangled name");
	}
	string ModifiedSymbol = pSymbol;
	ModifiedSymbol.insert(0, VFTableSymbolStart);
	ModifiedSymbol.insert(ModifiedSymbol.size(), VFTableSymbolEnd);
	if (!((UnDecorateSymbolName(ModifiedSymbol.c_str(), buff, 0x1000, 0)) != 0))
	{
		g_console.FWriteBold("Error Code: %d", GetLastError());
		return string(symbol); //Failsafe
	}
	return string(buff);
}

void StringFilter(string& string, const std::string& substring)
{
	size_t pos;
	while ((pos = string.find(substring)) != string::npos)
	{
		string.erase(pos, substring.length());
	}
}

void FilterSymbol(string& Symbol)
{
	vector<string> filters = 
	{
		"::`vftable'",
		"const ",
		"::`anonymous namespace'"
	};
	for (string filter : filters)
	{
		StringFilter(Symbol, filter);
	}
}


ofstream VTableLog;
ofstream InheritanceLog;
const size_t bufsize = 1024 * 1024;
char buf1[bufsize];
char buf2[bufsize];

void InitializeLogs()
{

	wstring vtable_path = GetDesktopPath();
	vtable_path.append(L"\\vtable.txt");
	VTableLog.open(vtable_path);

	wstring inheritance_path = GetDesktopPath();
	inheritance_path.append(L"\\inheritance.txt");
	InheritanceLog.open(inheritance_path);
	
	InheritanceLog.rdbuf()->pubsetbuf(buf1, bufsize);
	InheritanceLog.rdbuf()->pubsetbuf(buf2, bufsize);
}

void LogModuleStart(char* moduleName)
{
	VTableLog << "<" << moduleName << '>' << "\n";
	InheritanceLog << "<" << moduleName << '>' << "\n";
}

void LogModuleEnd(char* moduleName)
{
	VTableLog << "< end " << moduleName << '>' << "\n\n";
	InheritanceLog << "< end " << moduleName << '>' << "\n\n";
}

void CloseLogs()
{
	VTableLog.close();
	InheritanceLog.close();
}

void DumpVTableInfo(uintptr_t VTable, SectionInfo* sectionInfo)
{
	uintptr_t* vftable_ptr = reinterpret_cast<uintptr_t*>(VTable);
	uintptr_t* meta_ptr = vftable_ptr - 1;
	CompleteObjectLocator* COL = reinterpret_cast<CompleteObjectLocator*>(*meta_ptr);
#ifdef _WIN64
	TypeDescriptor* pTypeDescriptor = COL->GetTypeDescriptor();
	ClassHierarchyDescriptor* pClassDescriptor = COL->GetClassDescriptor();
#else
	TypeDescriptor* pTypeDescriptor = COL->pTypeDescriptor;
	ClassHierarchyDescriptor* pClassDescriptor = COL->pClassDescriptor;
#endif
	string className = DemangleMSVC(&pTypeDescriptor->name);
	auto FunctionList = VTHelper::GetListOfFunctions((void*)VTable, sectionInfo);
	bool MultipleInheritance = pClassDescriptor->attributes & 0b01;
	bool VirtualInheritance = pClassDescriptor->attributes & 0b10;
	char MH = (MultipleInheritance) ? 'M' : ' ';
	char VH = (VirtualInheritance) ? 'V' : ' ';
	VTableLog << MH << VH << hex << "0x" << VTable << "\t+" << GetRVA(VTable, sectionInfo) << "\t" << className << "\t" << "\n";
	int index = 0;
	if (!FunctionList.empty())
	{
		VTableLog << "\tVirtual Functions:\n";
		// Function Classification (Similar to IDA naming conventions)
		for (auto function : FunctionList) {
			VTableLog << "\t" << dec << index << "\t" << hex << "0x" << function << "\t+" << GetRVA(function, sectionInfo);
			BYTE* fnByte = (BYTE*) function;
			if (fnByte[0] == RET_INSTR) {
				VTableLog << "\t\tnullsub_" << hex << function << "\n";
			}
			else if(fnByte[0] == RET_INT_INSTR){
				WORD ret_integer = *(WORD*)&fnByte[1];
				VTableLog << "\t\tret" << ret_integer << "_" << hex << function << "\n";
			}
			else
			{
				VTableLog << "\t\tsub_" << hex << function << "\n";
			}
			index++;
		}
	}
	VTableLog << "\n\n";
}

void DumpInheritanceInfo(uintptr_t VTable, SectionInfo* sectionInfo)
{
	uintptr_t* vftable_ptr = reinterpret_cast<uintptr_t*>(VTable);
	uintptr_t* meta_ptr = vftable_ptr - 1;
	CompleteObjectLocator* COL = reinterpret_cast<CompleteObjectLocator*>(*meta_ptr);
#ifdef _WIN64
	TypeDescriptor* pTypeDescriptor = COL->GetTypeDescriptor();
	ClassHierarchyDescriptor* pClassDescriptor = COL->GetClassDescriptor();
	BaseClassArray* pClassArray = pClassDescriptor->GetBaseClassArray();
#else
	TypeDescriptor* pTypeDescriptor = COL->pTypeDescriptor;
	ClassHierarchyDescriptor* pClassDescriptor = COL->pClassDescriptor;
	BaseClassArray* pClassArray = pClassDescriptor->pBaseClassArray;
#endif
	unsigned long numBaseClasses = pClassDescriptor->numBaseClasses;
	string className = DemangleMSVC(&pTypeDescriptor->name);
	FilterSymbol(className);
	if (numBaseClasses > 1)\
	{
		InheritanceLog << className << ":" << "\n";
	}
	else
	{
		InheritanceLog << className << " (No Base Classes)" << "\n\n";
		return;
	}
	
	for (unsigned long i = 1; i < numBaseClasses; i++) {
#ifdef _WIN64
		BaseClassDescriptor* pCurrentBaseClass = pClassArray->GetBaseClassDescriptor(i);
		TypeDescriptor* pCurrentTypeDesc = pCurrentBaseClass->GetTypeDescriptor();
#else
		BaseClassDescriptor* pCurrentBaseClass = pClassArray->arrayOfBaseClassDescriptors[i];
		TypeDescriptor* pCurrentTypeDesc = pCurrentBaseClass->pTypeDescriptor;
#endif
		ptrdiff_t mdisp = pCurrentBaseClass->where.mdisp;
		ptrdiff_t pdisp = pCurrentBaseClass->where.pdisp;
		ptrdiff_t vdisp = pCurrentBaseClass->where.vdisp;

		string currentBaseClassName = DemangleMSVC(&pCurrentTypeDesc->name);
		FilterSymbol(currentBaseClassName);
		if (pdisp == -1) { // if pdisp is -1, the vtable offset for base class is actually mdisp
			InheritanceLog << hex << "0x" << mdisp << "\t";
		}
		// else, I dont know how to parse the vbtable yet;
		InheritanceLog << currentBaseClassName << "\n";
	}
	InheritanceLog << "\n";
}