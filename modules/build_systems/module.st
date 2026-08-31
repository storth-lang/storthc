#import "os";
#import "io";
#import "process";

enum_flag Build_Type{
    EXE;
    OBJ;
    ASM;
}

pub fn build(main:string,output:string,type:Build_Type=Build_Type.EXE)->bool{
    fn string_of_Build_type(t:Build_Type)->string{
    if t {
        case Build_Type.EXE :return "exe";
        case Build_Type.OBJ :return "obj";
        case Build_Type.ASM :return "asm";
    }
    return "";
    }
    cms:=["storthc", "build", string_of_Build_type(type), main, "-o", output];
    print("%\n",cms);

    proc, so :=runCommandProcess(cms,true,true);
    return (proc.exitCode==0);
}
