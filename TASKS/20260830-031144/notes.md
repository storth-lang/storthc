# Double_passing_error

- ID: 20260830-031144

## PRIORITY: 100

## STATUS: CLOSED

## TAGS: error, build, parser

## NOTES:

This error was found by CodeVoid basically double passing the array to the function errors with error code 127.

```
#import "io";
#import "process";
#import "string";

enum_flag BuildType {
    EXE,
    OBJ,
    DLL,
    SO,
}

fn build_type(b: BuildType) -> string {
    if b {
        case BuildType.EXE {return "exe"; }
        case BuildType.OBJ {return "obj";}
        default { return ""; }
    }
}

pub fn build_system_exec(comp: []string)->i32{
    foo,bar:=runCommandProcess(comp,true, true);

    if !bar {
        print("command % dose not exist",comp[0]);
        return 1;
    }

    print ("%",foo.capturedStdout);
    print ("%",foo.capturedStderr);

    return foo.exitCode ;
}

fn build(src_path: string, buildType : BuildType) -> bool {
    b := build_type(buildType);
    command := ["storthc", "build", "exe", src_path ];
    print("%\n", command);

    proc, ok := runCommandProcess(command);
    // print("%\n", proc);
    if !ok then return ok;

    if proc.exitCode != 0 {
        // print("%\n", proc.capturedStderr);
        return false;
    }

    // if capturestdout {
    //     print("%\n", proc.capturedStdout);
    // }
    return true;
}

pub fn main(args: []string) -> i32 {
    proc, ok := runCommandProcess(["storthc", "build", "exe" ,"./main.st"]);
    if !ok then return 1;

    print("%\n", proc.exitCode);


    return 0;
}

```
