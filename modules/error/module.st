pub enum_flag memoryError {
    None;
	ReserveFailed;
	ProtectFailed;
	CommitFailed;
	OutOfBounds;
}

pub enum_flag ioError {
    None;
	OpenErrorC;
	OpenErrorR;
	OpenErrorRW;
	OpenErrorW;
	WriteError;
	ReadError;
}

pub fn getErrorString(err: $T(ioError | memoryError)) -> string {
	if err {
		case memoryError.ReserveFailed { return "failed to reserve address space (mmap)"; }
		case memoryError.ProtectFailed { return "failed to make the initial arena page read/write (mprotect)"; }
		case memoryError.CommitFailed { return "failed to commit additional arena pages (mprotect)"; }
		case memoryError.OutOfBounds { return "allocation exceeds the arena's reserved size"; }
		case ioError.OpenErrorC { return "could not open fd for creation"; }
		case ioError.OpenErrorW { return "could not open fd for writing"; }
		case ioError.OpenErrorR { return "could not open fd for reading"; }
		case ioError.OpenErrorRW { return "could not open fd for both reading and writing"; }
		case ioError.WriteError { return "could not write into file descriptor"; }
		case ioError.ReadError { return "could not read from file descriptor"; }
		default { return "unknown error kind"; }
	}
}

pub fn errorReport(err: $T(ioError | memoryError)) -> void {
    eprint("%\n", getErrorString(err));
}

pub fn errnoString(e: i64) -> string {
	if e == 1 then return "Operation not permitted";
	if e == 2 then return "No such file or directory";
	if e == 4 then return "Interrupted system call";
	if e == 5 then return "Input/output error";
	if e == 9 then return "Bad file descriptor";
	if e == 11 then return "Resource temporarily unavailable";
	if e == 12 then return "Cannot allocate memory";
	if e == 13 then return "Permission denied";
	if e == 14 then return "Bad address";
	if e == 17 then return "File exists";
	if e == 20 then return "Not a directory";
	if e == 21 then return "Is a directory";
	if e == 22 then return "Invalid argument";
	if e == 24 then return "Too many open files";
	if e == 27 then return "File too large";
	if e == 28 then return "No space left on device";
	if e == 30 then return "Read-only file system";
	if e == 32 then return "Broken pipe";
	if e == 36 then return "File name too long";
	return "unknown errno";
}

#import "io";
