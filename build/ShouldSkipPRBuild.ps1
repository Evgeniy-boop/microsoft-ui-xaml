﻿function AllChangedFilesAreSkippable
{
    Param($files)

    #$skipExts = @(".md")
    $skipExts = @(".md", ".ps1", ".yml")
    $allFilesAreSkippable = $true

    foreach($file in $files)
    {
        Write-Host "Checking '$file'"
        $ext = (Get-Item $file).Extension
        $fileIsSkippable = $ext -in $skipExts
        Write-Host "File '$file' is skippable: '$fileIsSkippable'"

        if(!$fileIsSkippable)
        {
            $allFilesAreSkippable = $false
        }
    }

    return $allFilesAreSkippable
}

$shouldSkipBuild = $false

#if($env:BUILD_REASON -eq "PullRequest")
#{
    #$targetBranch = $env:SYSTEM_PULLREQUEST_TARGETBRANCH
    $targetBranch = "master"

    $diffOutput = & git diff $targetBranch --name-only
    $files = $diffOutput.Split([Environment]::NewLine)

    Write-Host "Files changed: $files"
    

    $shouldSkipBuild = AllChangedFilesAreSkippable($files)
#}


Write-Host $shouldSkipBuild

Write-Host "##vso[task.setvariable variable=shouldSkipPRBuild]$shouldSkipPRBuild"